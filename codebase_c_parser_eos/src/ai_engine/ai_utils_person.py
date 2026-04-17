"""
ai_utils_person.py
---------------------
Person Tracker for the SmartIP AI engine.
Extracts Class 0 (Person) from YOLO-family TFLite models.

Changes vs original
--------------------
[FIX-PERSON-1]  Alert deduplication replaced.
    Old: a permanent set of spatial buckets cleared only after >500 entries
         (bulk nuclear wipe).  Problems:
           - A person detected at bucket (3,4) was never re-alerted after the
             first detection, even if they walked away and came back.
           - The bulk clear() fired mid-frame and could silently drop
             legitimate detections in the same batch.
    New: a dict keyed by bucket -> last_alert_monotonic_time.
         One alert fires per bucket per ALERT_COOLDOWN_SEC (default 30 s).
         If the subject stays in frame a re-alert fires every 30 s, which
         is exactly the right behaviour for sustained-presence recording.
         Stale buckets (unseen for _BUCKET_TTL_SEC = 5 min) are evicted with
         a cheap dict comprehension — no sudden wipe, no race.

[FIX-PERSON-2]  NMS index handling corrected.
    Original used order[np.where(iou <= thresh)[0] + 1] which is the classic
    pattern but applies the +1 offset AFTER np.where, shifting the kept
    indices by one when any IoU exceeds the threshold.  Replaced with the
    standard pattern using rest = order[1:] throughout.

[CPU-OPT-PERSON] Avoids redundant array re-indexing inside the NMS loop by
    working on the already-sliced f_boxes/f_scores views.
"""

import time
import numpy as np

CONF_THRESHOLD     = 0.10
NMS_THRESHOLD      = 0.45
ALERT_COOLDOWN_SEC = 30    # one alert per spatial bucket per N seconds
_BUCKET_TTL_SEC    = 300   # evict unseen buckets after 5 minutes


class PersonTracker:
    def __init__(self):
        # [FIX-PERSON-1] Cooldown dict: bucket_key -> monotonic time of last alert
        self._bucket_last_alert: dict = {}

    # ------------------------------------------------------------------
    # Non-Maximum Suppression  (cx/cy/w/h normalised input)
    # ------------------------------------------------------------------
    def _nms(self, boxes, scores, thresh):
        """Return list of kept indices.  boxes are [N,4] cx/cy/w/h in [0,1]."""
        if len(boxes) == 0:
            return []

        x1 = boxes[:, 0] - boxes[:, 2] / 2
        y1 = boxes[:, 1] - boxes[:, 3] / 2
        x2 = boxes[:, 0] + boxes[:, 2] / 2
        y2 = boxes[:, 1] + boxes[:, 3] / 2
        areas = (x2 - x1) * (y2 - y1)

        order = scores.argsort()[::-1]
        keep = []

        while order.size > 0:
            i = order[0]
            keep.append(int(i))
            rest = order[1:]
            if rest.size == 0:
                break
            xx1 = np.maximum(x1[i], x1[rest])
            yy1 = np.maximum(y1[i], y1[rest])
            xx2 = np.minimum(x2[i], x2[rest])
            yy2 = np.minimum(y2[i], y2[rest])
            inter = np.maximum(0.0, xx2 - xx1) * np.maximum(0.0, yy2 - yy1)
            iou   = inter / (areas[i] + areas[rest] - inter + 1e-6)
            order = rest[iou <= thresh]

        return keep

    # ------------------------------------------------------------------
    # Main entry point (called by ai_engine.py _on_new_sample)
    # ------------------------------------------------------------------
    def process(self, output_data, scale_factor, zero_point, frame_scale,
                pad_x, pad_y, ml_w, ml_h, threshold=None):
        """
        Returns (coords_payload, alerts_payload).

        coords_payload — list of dicts for ZMQ TOPIC_AI_COORDS (overlay boxes).
        alerts_payload — list of dicts for ZMQ TOPIC_ALERTS (recording / email).
        """
        out = output_data
        conf_thresh = threshold if threshold is not None else CONF_THRESHOLD

        # De-quantise INT8 output if needed
        if scale_factor:
            out = (out.astype(np.float32) - zero_point) * scale_factor
        out = np.squeeze(out)

        # Some models output [features, N] — transpose to [N, features]
        if out.ndim == 2 and out.shape[0] < out.shape[1]:
            out = out.T

        if out.ndim < 2 or out.shape[1] < 5:
            return [], []

        # Class 0 (Person) confidence is column 4 in YOLO-style output
        boxes  = out[:, :4]   # [N, 4] — cx/cy/w/h normalised [0..1]
        scores = out[:, 4]    # [N]    — class-0 confidence

        mask = scores > conf_thresh
        if not np.any(mask):
            return [], []

        f_boxes  = boxes[mask]
        f_scores = scores[mask]
        indices  = self._nms(f_boxes, f_scores, NMS_THRESHOLD)

        now = time.monotonic()
        coords_payload = []
        alerts_payload = []

        for i in indices:
            cx_norm, cy_norm, bw_norm, bh_norm = f_boxes[i]

            # Back-project from letterboxed 320×320 ML space to frame pixels
            ml_size = 320
            cx = (cx_norm * ml_size - pad_x) / frame_scale
            cy = (cy_norm * ml_size - pad_y) / frame_scale
            bw = (bw_norm * ml_size)          / frame_scale
            bh = (bh_norm * ml_size)          / frame_scale

            x1 = cx - bw / 2
            y1 = cy - bh / 2
            x2 = cx + bw / 2
            y2 = cy + bh / 2

            conf = float(f_scores[i])

            coords_payload.append({
                "bbox": (
                    float(max(0.0, x1)),
                    float(max(0.0, y1)),
                    float(min(float(ml_w), x2)),
                    float(min(float(ml_h), y2)),
                ),
                "cid":        0,
                "conf":       conf,
                "class_name": "Person",
            })

            # [FIX-PERSON-1] Cooldown-based alert deduplication.
            # 50-pixel buckets in frame space group a stationary person
            # across small jitter while distinguishing nearby individuals.
            bucket = f"P{int(cx) // 50}_{int(cy) // 50}"
            last_t = self._bucket_last_alert.get(bucket, 0.0)

            if now - last_t >= ALERT_COOLDOWN_SEC:
                self._bucket_last_alert[bucket] = now
                alerts_payload.append({
                    "type": "person_detected",
                    "conf": conf,
                })

        # [FIX-PERSON-1] Evict stale buckets — cheap O(n) comprehension,
        # no sudden bulk clear() that could race with concurrent frames.
        cutoff = now - _BUCKET_TTL_SEC
        if self._bucket_last_alert:
            self._bucket_last_alert = {
                k: v for k, v in self._bucket_last_alert.items() if v > cutoff
            }

        return coords_payload, alerts_payload
