"""
ai_utils_helmet.py
---------------------
Postprocessing and StableTracker for Helmet/License Plate Detection.
Adapted from helmet.py to plug into ai_engine.py.
"""
import numpy as np
from collections import OrderedDict, deque
import time

CONF_THRESHOLD = 0.20
NMS_THRESHOLD = 0.45

# Tracking parameters
SMOOTHING_FRAMES = 5
MAX_DISAPPEARED = 8
IOU_THRESHOLD = 0.4
MIN_STABLE_CONFIDENCE = 0.25

class StableTracker:
    """IoU-based tracker with temporal smoothing for stable class prediction"""
    
    def __init__(self, max_disappeared=MAX_DISAPPEARED, smoothing_frames=SMOOTHING_FRAMES):
        self.nextObjectID = 0
        self.objects = OrderedDict()
        self.disappeared = OrderedDict()
        self.max_disappeared = max_disappeared
        self.class_history = OrderedDict()
        self.confidence_history = OrderedDict()
        self.smoothing_frames = smoothing_frames
        
    def calculate_iou(self, box1, box2):
        x1_1, y1_1, x2_1, y2_1 = box1
        x1_2, y1_2, x2_2, y2_2 = box2
        xi1, yi1 = max(x1_1, x1_2), max(y1_1, y1_2)
        xi2, yi2 = min(x2_1, x2_2), min(y2_1, y2_2)
        inter_area = max(0, xi2 - xi1) * max(0, yi2 - yi1)
        box1_area = (x2_1 - x1_1) * (y2_1 - y1_1)
        box2_area = (x2_2 - x1_2) * (y2_2 - y1_2)
        union_area = box1_area + box2_area - inter_area
        return inter_area / union_area if union_area > 0 else 0

    def register(self, bbox, class_id, confidence):
        objectID = self.nextObjectID
        self.objects[objectID] = bbox
        self.disappeared[objectID] = 0
        self.class_history[objectID] = deque(maxlen=self.smoothing_frames)
        self.confidence_history[objectID] = deque(maxlen=self.smoothing_frames)
        self.class_history[objectID].append(class_id)
        self.confidence_history[objectID].append(confidence)
        self.nextObjectID += 1
        return objectID

    def deregister(self, objectID):
        del self.objects[objectID]
        del self.disappeared[objectID]
        if objectID in self.class_history: del self.class_history[objectID]
        if objectID in self.confidence_history: del self.confidence_history[objectID]

    def get_stable_class(self, objectID):
        if objectID not in self.class_history: return None, 0
        history = list(self.class_history[objectID])
        confidences = list(self.confidence_history[objectID])
        if not history: return None, 0
        class_weights = {}
        for cls, conf in zip(history, confidences):
            class_weights[cls] = class_weights.get(cls, 0) + conf
        stable_class = max(class_weights, key=class_weights.get)
        avg_conf = float(np.mean([c for cls, c in zip(history, confidences) if cls == stable_class]))
        return stable_class, avg_conf

    def update(self, detections):
        if len(detections) == 0:
            for objectID in list(self.disappeared.keys()):
                self.disappeared[objectID] += 1
                if self.disappeared[objectID] > self.max_disappeared:
                    self.deregister(objectID)
            return self._get_results()

        boxes = [d['bbox'] for d in detections]
        class_ids = [d['cid'] for d in detections]
        confidences = [d['conf'] for d in detections]

        if len(self.objects) == 0:
            for bbox, cid, conf in zip(boxes, class_ids, confidences):
                self.register(bbox, cid, conf)
        else:
            objectIDs = list(self.objects.keys())
            existing_boxes = list(self.objects.values())
            iou_matrix = np.zeros((len(existing_boxes), len(boxes)))
            for i, eb in enumerate(existing_boxes):
                for j, nb in enumerate(boxes):
                    iou_matrix[i, j] = self.calculate_iou(eb, nb)
            
            matched_existing, matched_new = set(), set()
            matches = [(iou_matrix[i, j], i, j) for i in range(len(existing_boxes)) 
                       for j in range(len(boxes)) if iou_matrix[i, j] > IOU_THRESHOLD]
            matches.sort(reverse=True, key=lambda x: x[0])
            
            for iou, i, j in matches:
                if i not in matched_existing and j not in matched_new:
                    objectID = objectIDs[i]
                    self.objects[objectID] = boxes[j]
                    self.disappeared[objectID] = 0
                    self.class_history[objectID].append(class_ids[j])
                    self.confidence_history[objectID].append(confidences[j])
                    matched_existing.add(i)
                    matched_new.add(j)
            
            for i, objectID in enumerate(objectIDs):
                if i not in matched_existing:
                    self.disappeared[objectID] += 1
                    if self.disappeared[objectID] > self.max_disappeared:
                        self.deregister(objectID)
            
            for j in range(len(boxes)):
                if j not in matched_new:
                    self.register(boxes[j], class_ids[j], confidences[j])
        
        return self._get_results()

    def _get_results(self):
        results = {}
        for objectID in self.objects.keys():
            if self.disappeared.get(objectID, 0) == 0:
                stable_class, avg_conf = self.get_stable_class(objectID)
                if stable_class is not None and avg_conf >= MIN_STABLE_CONFIDENCE:
                    bbox = self.objects[objectID]
                    results[objectID] = {'bbox': tuple(float(v) for v in bbox), 'cid': int(stable_class), 'conf': float(avg_conf)}
        return results

class HelmetTracker:
    def __init__(self):
        self.tracker = StableTracker()
        self.saved_plates = set() # Store hashes
        self.saved_violations = set()
        
    def _nms(self, boxes, scores, thresh):
        if len(boxes) == 0:
            return []
        x1, y1 = boxes[:, 0] - boxes[:, 2]/2, boxes[:, 1] - boxes[:, 3]/2
        x2, y2 = boxes[:, 0] + boxes[:, 2]/2, boxes[:, 1] + boxes[:, 3]/2
        areas = (x2 - x1) * (y2 - y1)
        order = scores.argsort()[::-1]
        keep = []
        while order.size > 0:
            i = order[0]
            keep.append(i)
            xx1, yy1 = np.maximum(x1[i], x1[order[1:]]), np.maximum(y1[i], y1[order[1:]])
            xx2, yy2 = np.minimum(x2[i], x2[order[1:]]), np.minimum(y2[i], y2[order[1:]])
            inter = np.maximum(0, xx2-xx1) * np.maximum(0, yy2-yy1)
            iou = inter / (areas[i] + areas[order[1:]] - inter + 1e-6)
            order = order[np.where(iou <= thresh)[0] + 1]
        return keep

    def process(self, output_data, scale_factor, zero_point, frame_scale, pad_x, pad_y, ml_w, ml_h, threshold=None):
        out = output_data
        conf_thresh = threshold if threshold is not None else CONF_THRESHOLD
        if scale_factor: 
            out = (out.astype(np.float32) - zero_point) * scale_factor
        out = np.squeeze(out)
        if out.ndim == 2 and out.shape[0] < out.shape[1]: out = out.T
        if out.ndim < 2 or out.shape[1] < 5: 
            return [], []
            
        boxes = out[:, :4]
        scores = out[:, 4:7].max(axis=1)
        classes = out[:, 4:7].argmax(axis=1)
        
        mask = scores > conf_thresh
        if not np.any(mask): 
            return [], []
            
        f_boxes, f_scores, f_classes = boxes[mask], scores[mask], classes[mask]
        indices = self._nms(f_boxes, f_scores, NMS_THRESHOLD)
        
        raw_detections = []
        for i in indices:
            cx_norm, cy_norm, bw_norm, bh_norm = f_boxes[i]
            
            # 320 is the ML size
            cx = (cx_norm * 320 - pad_x) / frame_scale
            cy = (cy_norm * 320 - pad_y) / frame_scale
            bw = (bw_norm * 320) / frame_scale
            bh = (bh_norm * 320) / frame_scale
            
            # Map back to ml_w, ml_h size
            x1 = (cx - bw/2) 
            y1 = (cy - bh/2) 
            x2 = (cx + bw/2) 
            y2 = (cy + bh/2) 
            
            raw_detections.append({
                'bbox': (float(max(0, x1)), float(max(0, y1)), float(min(ml_w, x2)), float(min(ml_h, y2))),
                'cid': int(f_classes[i]),
                'conf': float(f_scores[i])
            })
            
        tracked = self.tracker.update(raw_detections)
        
        coords_payload = []
        alerts_payload = []
        
        # Plates (cid=2) are not reliably tracked by StableTracker since it's IoU based and they move fast/small
        # So we include raw plates directly.
        for d in raw_detections:
             if d['cid'] == 2:
                 coords_payload.append({
                     "bbox": d['bbox'],
                     "cid": 2,
                     "conf": d['conf'],
                     "tid": "P"
                 })

        for tid, d in tracked.items():
            coords_payload.append({
                "bbox": d['bbox'],
                "cid": d['cid'],
                "conf": d['conf'],
                "tid": tid
            })
            
            # No helmet violation (cid == 1)
            # Send alert if we see a violator with a plate nearby (or just violators in general)
            if d['cid'] == 1:
                # To prevent spamming alerts for the same person, we check if we've sent
                h = f"V{tid}"
                if h not in self.saved_violations:
                     self.saved_violations.add(h)
                     alerts_payload.append({
                         "type": "no_helmet",
                         "tid": tid,
                         "conf": d['conf']
                     })
             
        # Cleanup saved violations to avoid mem leak
        if len(self.saved_violations) > 1000:
             self.saved_violations.clear()

        return coords_payload, alerts_payload
