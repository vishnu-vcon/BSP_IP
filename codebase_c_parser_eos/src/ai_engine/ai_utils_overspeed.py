"""
ai_utils_overspeed.py
---------------------
Postprocessing and Centroid Tracker for Overspeeding Detection.
Adapted from overspeed_rtsp.py to plug into ai_engine.py cleanly.
"""
import numpy as np
from collections import OrderedDict
import time

CONF_THRESHOLD = 0.3
NMS_THRESHOLD = 0.45
PIXELS_PER_METER = 25
SPEED_LIMIT_KMH = 60
VEHICLE_CLASS_IDS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]

class OptimizedPostprocessor:
    """Fast vectorized postprocessing using NumPy"""
    def __init__(self, conf_threshold=0.3, nms_threshold=0.45, topk=500, vehicle_class_ids=None):
        self.conf_threshold = conf_threshold
        self.nms_threshold = nms_threshold
        self.topk = topk
        self.vehicle_class_ids = vehicle_class_ids or VEHICLE_CLASS_IDS
    
    def _nms_numpy(self, boxes, scores, iou_threshold):
        if boxes.shape[0] == 0:
            return np.array([], dtype=int)
        x1, y1, x2, y2 = boxes[:, 0], boxes[:, 1], boxes[:, 2], boxes[:, 3]
        areas = (x2 - x1 + 1) * (y2 - y1 + 1)
        order = scores.argsort()[::-1]
        keep = []
        while order.size > 0:
            i = order[0]
            keep.append(i)
            if order.size == 1:
                break
            xx1 = np.maximum(x1[i], x1[order[1:]])
            yy1 = np.maximum(y1[i], y1[order[1:]])
            xx2 = np.minimum(x2[i], x2[order[1:]])
            yy2 = np.minimum(y2[i], y2[order[1:]])
            w = np.maximum(0.0, xx2 - xx1 + 1)
            h = np.maximum(0.0, yy2 - yy1 + 1)
            inter = w * h
            iou = inter / (areas[i] + areas[order[1:]] - inter)
            inds = np.where(iou <= iou_threshold)[0]
            order = order[inds + 1]
        return np.array(keep, dtype=int)
    
    def process(self, output_data, frame_shape, input_shape, scale_params, scale, pad_x, pad_y):
        out = output_data
        if scale_params and scale_params.get('scale') is not None:
            out = (out.astype(np.float32) - scale_params['zero_point']) * scale_params['scale']
        
        if len(out.shape) == 3:
            out = np.squeeze(out).T
        else:
            out = np.squeeze(out)
        
        if out.ndim == 1:
            out = np.expand_dims(out, 0)
        if out.shape[0] == 0 or out.shape[1] < 5:
            return [], [], []
        
        frame_w, frame_h = frame_shape # e.g. 640, 360
        in_w, in_h = input_shape # e.g. 320, 320
        
        class_scores = out[:, 4:]
        max_scores = class_scores.max(axis=1)
        class_ids = class_scores.argmax(axis=1)
        
        conf_mask = max_scores > self.conf_threshold
        if not np.any(conf_mask):
            return [], [], []
        
        # Filter by vehicle classes
        valid_indices = np.where(conf_mask)[0]
        valid_indices = valid_indices[np.isin(class_ids[valid_indices], self.vehicle_class_ids)]
        if len(valid_indices) == 0:
            return [], [], []

        sel_scores = max_scores[valid_indices]
        sel_cls = class_ids[valid_indices]
        sel_preds = out[valid_indices, :4]
        
        if sel_scores.shape[0] > self.topk:
            topk_inds = np.argpartition(-sel_scores, self.topk)[:self.topk]
            sel_scores = sel_scores[topk_inds]
            sel_cls = sel_cls[topk_inds]
            sel_preds = sel_preds[topk_inds]
        
        # Scale back from ML to Input Res
        cx = (sel_preds[:, 0] * in_w - pad_x) / scale
        cy = (sel_preds[:, 1] * in_h - pad_y) / scale
        bw = (sel_preds[:, 2] * in_w) / scale
        bh = (sel_preds[:, 3] * in_h) / scale
        
        x1 = np.clip(cx - bw / 2.0, 0, frame_w - 1)
        y1 = np.clip(cy - bh / 2.0, 0, frame_h - 1)
        x2 = np.clip(cx + bw / 2.0, 0, frame_w - 1)
        y2 = np.clip(cy + bh / 2.0, 0, frame_h - 1)
        
        boxes = np.stack([x1, y1, x2, y2], axis=1).astype(np.float32)
        keep = self._nms_numpy(boxes, sel_scores.astype(np.float32), self.nms_threshold)
        
        if keep.size == 0:
            return [], [], []
        
        return boxes[keep].tolist(), sel_scores[keep].tolist(), sel_cls[keep].tolist()

class CentroidTracker:
    def __init__(self, maxDisappeared=15):
        self.nextObjectID = 0
        self.objects = OrderedDict()
        self.disappeared = OrderedDict()
        self.maxDisappeared = maxDisappeared
        self.violation_status = {}

    def register(self, centroid):
        self.objects[self.nextObjectID] = centroid
        self.disappeared[self.nextObjectID] = 0
        self.violation_status[self.nextObjectID] = False
        self.nextObjectID += 1

    def deregister(self, objectID):
        del self.objects[objectID]
        del self.disappeared[objectID]
        if objectID in self.violation_status:
            del self.violation_status[objectID]
        # Notify parent tracker to clean up history
        if hasattr(self, '_on_deregister') and self._on_deregister:
            self._on_deregister(objectID)

    def update(self, boxes):
        if len(boxes) == 0:
            for objectID in list(self.disappeared.keys()):
                self.disappeared[objectID] += 1
                if self.disappeared[objectID] > self.maxDisappeared:
                    self.deregister(objectID)
            return self.objects

        inputCentroids = np.zeros((len(boxes), 2), dtype="int")
        for (i, box) in enumerate(boxes):
            if isinstance(box, (list, tuple)) and len(box) == 4:
                x1, y1, x2, y2 = box
            else:
                x1, y1, x2, y2 = box[0], box[1], box[2], box[3]
            cX = int((x1 + x2) / 2.0)
            cY = int((y1 + y2) / 2.0)
            inputCentroids[i] = (cX, cY)

        if len(self.objects) == 0:
            for i in range(len(inputCentroids)):
                self.register(inputCentroids[i])
        else:
            objectIDs = list(self.objects.keys())
            objectCentroids = list(self.objects.values())
            D = np.linalg.norm(np.array(objectCentroids)[:, np.newaxis] - inputCentroids, axis=2)
            rows = D.min(axis=1).argsort()
            cols = D.argmin(axis=1)[rows]
            usedRows, usedCols = set(), set()
            for (row, col) in zip(rows, cols):
                if row in usedRows or col in usedCols:
                    continue
                objectID = objectIDs[row]
                self.objects[objectID] = inputCentroids[col]
                self.disappeared[objectID] = 0
                usedRows.add(row)
                usedCols.add(col)
            unusedRows = set(range(0, D.shape[0])).difference(usedRows)
            unusedCols = set(range(0, D.shape[1])).difference(usedCols)
            if D.shape[0] >= D.shape[1]:
                for row in unusedRows:
                    objectID = objectIDs[row]
                    self.disappeared[objectID] += 1
                    if self.disappeared[objectID] > self.maxDisappeared:
                        self.deregister(objectID)
            else:
                for col in unusedCols:
                    self.register(inputCentroids[col])
        return self.objects

    def mark_violation(self, objectID):
        if objectID in self.violation_status:
            self.violation_status[objectID] = True

    def is_violating(self, objectID):
        return self.violation_status.get(objectID, False)


class OverspeedTracker:
    """Wrapper that adapts overspeed logic to ai_engine output format."""
    def __init__(self):
        self.postproc = OptimizedPostprocessor()
        self.tracker = CentroidTracker(maxDisappeared=15)
        self.track_history = {}
        self.saved_violations = set()
        self.actual_fps = 30 # Assumption since we're receiving decoded frames around 30fps
        # Register cleanup callback
        self.tracker._on_deregister = self._cleanup_object

    def _cleanup_object(self, objectID):
        """Free memory for deregistered tracked objects."""
        self.track_history.pop(objectID, None)
        self.saved_violations.discard(objectID)

    def process(self, output, scale_factor, zero_point, frame_scale, pad_x, pad_y, ml_w, ml_h, threshold=None):
        if threshold is not None:
             self.postproc.conf_threshold = threshold
        scale_params = None
        if scale_factor is not None:
            scale_params = {'scale': scale_factor, 'zero_point': zero_point}
            
        boxes, scores, classes = self.postproc.process(
            output, (ml_w, ml_h), (320, 320), scale_params, frame_scale, pad_x, pad_y
        )
        
        tracked_objects = self.tracker.update(boxes)
        coords_payload = []
        alerts_payload = []
        
        for (objectID, centroid) in tracked_objects.items():
            if objectID not in self.track_history:
                self.track_history[objectID] = []
            self.track_history[objectID].append(centroid)
            
            speed_kmh = 0
            # Calculate speed based on previous frames
            if len(self.track_history[objectID]) > 5:
                pixel_dist = np.linalg.norm(
                    np.array(self.track_history[objectID][-1]) -
                    np.array(self.track_history[objectID][-5])
                )
                meter_dist = pixel_dist / PIXELS_PER_METER
                time_sec = 4.0 / float(self.actual_fps)
                if time_sec > 0:
                    speed_kmh = (meter_dist / time_sec) * 3.6
                
            is_violating = False
            if speed_kmh > SPEED_LIMIT_KMH:
                if objectID not in self.saved_violations:
                    self.tracker.mark_violation(objectID)
                    self.saved_violations.add(objectID)
                    alerts_payload.append({
                        "type": "overspeed",
                        "speed": speed_kmh,
                        "limit": SPEED_LIMIT_KMH,
                        "tid": objectID
                    })
                is_violating = True
                
            # Find the box for this centroid (heuristic)
            best_box = [0,0,0,0]
            for box in boxes:
                x1, y1, x2, y2 = box
                c = ((x1+x2)//2, (y1+y2)//2)
                if np.linalg.norm(np.array(c) - np.array(centroid)) < 50:
                    best_box = box
                    break
                    
            coords_payload.append({
                "bbox": best_box,
                "speed": speed_kmh,
                "tid": objectID,
                "violating": is_violating
            })
            
        return coords_payload, alerts_payload
