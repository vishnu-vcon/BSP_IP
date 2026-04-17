"""
ai_utils_triding.py
---------------------
Postprocessing and StableTracker for Triple Riding Detection.
Adapted from triding.py to plug into ai_engine.py.
"""
import numpy as np
from collections import OrderedDict, deque

CONF_THRESHOLD = 0.15
NMS_THRESHOLD = 0.45

# Triple Riding Detection Classes
MOTORCYCLE_CLASS_ID = 1
PERSON_CLASS_ID = 0

# Triple Riding Detection Parameters
MIN_PERSONS_PER_MC = 2
RIDER_IOA_THRESHOLD = 0.30
MC_EXPANSION_RATIO_X = 0.7
MC_EXPANSION_RATIO_Y_TOP = 2.5

# Size-based parameters
PERSON_MIN_HEIGHT = 70
PERSON_TO_MC_HEIGHT_RATIO = 1.9
PERSON_ASPECT_RATIO_MAX_TALL = 0.80
PERSON_ASPECT_RATIO_MIN_WIDE = 1.5

# Tracking parameters
SMOOTHING_FRAMES = 5
MAX_DISAPPEARED = 30
IOU_THRESHOLD = 0.5

def calculate_ioa(box1, box2):
    """Calculate Intersection over Area (person box)"""
    x1_1, y1_1, x2_1, y2_1 = box1
    x1_2, y1_2, x2_2, y2_2 = box2
    xi1 = max(x1_1, x1_2)
    yi1 = max(y1_1, y1_2)
    xi2 = min(x2_1, x2_2)
    yi2 = min(y2_1, y2_2)
    inter_area = max(0, xi2 - xi1) * max(0, yi2 - yi1)
    person_area = (x2_2 - x1_2) * (y2_2 - y1_2)
    return inter_area / person_area if person_area > 0 else 0.0

def get_union_bbox(boxes):
    """Get union bounding box"""
    if not boxes:
        return []
    x_min = min(box[0] for box in boxes)
    y_min = min(box[1] for box in boxes)
    x_max = max(box[2] for box in boxes)
    y_max = max(box[3] for box in boxes)
    return [x_min, y_min, x_max, y_max]

def get_box_area(box):
    return (box[2] - box[0]) * (box[3] - box[1])

def is_oversized_person_box(person_bbox, mc_bbox=None):
    """Check if person box is oversized"""
    px1, py1, px2, py2 = person_bbox
    p_width = px2 - px1
    p_height = py2 - py1
    
    if p_height < PERSON_MIN_HEIGHT:
        return False, f"Height too small: {p_height}px"
    
    aspect_ratio = p_width / p_height if p_height > 0 else 0
    
    if aspect_ratio < PERSON_ASPECT_RATIO_MAX_TALL:
        return True, f"Tall box (Aspect={aspect_ratio:.2f})"
    
    if aspect_ratio > PERSON_ASPECT_RATIO_MIN_WIDE:
        return True, f"Wide box (Aspect={aspect_ratio:.2f})"
    
    if mc_bbox is not None:
        mx1, my1, mx2, my2 = mc_bbox
        mc_height = my2 - my1
        
        if mc_height > 20:
            height_ratio = p_height / mc_height
            if height_ratio > PERSON_TO_MC_HEIGHT_RATIO:
                return True, f"Taller than MC (Ratio={height_ratio:.2f})"
    
    return False, "Normal size"

def detect_triple_riding_mra_hybrid(motorcycles, persons, img_w, img_h):
    """MRA HYBRID DETECTION - Core triple riding detection logic"""
    triple_riding_detections = []
    
    for person in persons:
        person['used'] = False
    
    for mc_idx, mc in enumerate(motorcycles):
        mc_bbox = mc['bbox']
        mc_score = mc['score']
        mx1, my1, mx2, my2 = mc_bbox
        mc_width = mx2 - mx1
        mc_height = my2 - my1
        
        expand_x = int(mc_width * MC_EXPANSION_RATIO_X)
        expand_y_top = int(mc_height * MC_EXPANSION_RATIO_Y_TOP)
        
        expanded_mx1 = max(0, mx1 - expand_x)
        expanded_my1 = max(0, my1 - expand_y_top)
        expanded_mx2 = min(img_w - 1, mx2 + expand_x)
        expanded_my2 = min(img_h - 1, my2)
        
        expanded_bbox = [expanded_mx1, expanded_my1, expanded_mx2, expanded_my2]
        
        associated_persons = []
        
        for person in persons:
            if person['used']:
                continue
                
            px1, py1, px2, py2 = person['bbox']
            p_height = py2 - py1
            
            if p_height < PERSON_MIN_HEIGHT:
                continue

            overlap_ioa = calculate_ioa(expanded_bbox, person['bbox'])
            
            if overlap_ioa >= RIDER_IOA_THRESHOLD:
                associated_persons.append(person)
                person['used'] = True
        
        num_associated = len(associated_persons)
        is_violation = False
        reason = ""
        riders = 0
        violation_score = 0.0
        
        if num_associated >= MIN_PERSONS_PER_MC:
            is_violation = True
            riders = num_associated + 1
            reason = f"Multiple persons: {num_associated}"
            person_scores = [p['score'] for p in associated_persons]
            violation_score = float(np.mean([mc_score] + person_scores))
            
        elif num_associated == 1:
            person = associated_persons[0]
            is_oversized, size_reason = is_oversized_person_box(person['bbox'], mc_bbox)
            
            if is_oversized:
                is_violation = True
                riders = 3
                reason = f"Oversized box: {size_reason}"
                violation_score = float(np.mean([mc_score, person['score']]))
        
        if is_violation:
            all_boxes = [mc_bbox] + [p['bbox'] for p in associated_persons]
            union_bbox = get_union_bbox(all_boxes)
            
            triple_riding_detections.append({
                'mc_bbox': mc_bbox,
                'riders': riders,
                'union_bbox': union_bbox,
                'rider_boxes': [p['bbox'] for p in associated_persons],
                'detection_method': 'MRA-hybrid',
                'reason': reason,
                'score': violation_score
            })
    
    return triple_riding_detections


class StableTracker:
    def __init__(self, max_disappeared=MAX_DISAPPEARED, smoothing_frames=SMOOTHING_FRAMES):
        self.nextObjectID = 0
        self.objects = OrderedDict()
        self.disappeared = OrderedDict()
        self.max_disappeared = max_disappeared
        self.data_history = OrderedDict()
        self.confidence_history = OrderedDict()
        self.smoothing_frames = smoothing_frames
        self.best_violation_data = OrderedDict()
        
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

    def register(self, detection_data):
        objectID = self.nextObjectID
        self.objects[objectID] = detection_data['union_bbox']
        self.disappeared[objectID] = 0
        self.data_history[objectID] = deque(maxlen=self.smoothing_frames)
        self.confidence_history[objectID] = deque(maxlen=self.smoothing_frames)
        self.data_history[objectID].append(detection_data)
        self.confidence_history[objectID].append(detection_data['score'])
        self.best_violation_data[objectID] = detection_data.copy()
        self.nextObjectID += 1
        return objectID

    def deregister(self, objectID):
        del self.objects[objectID]
        del self.disappeared[objectID]
        if objectID in self.data_history: del self.data_history[objectID]
        if objectID in self.confidence_history: del self.confidence_history[objectID]
        if objectID in self.best_violation_data: del self.best_violation_data[objectID]

    def get_stable_data(self, objectID):
        if objectID not in self.data_history or len(self.data_history[objectID]) == 0:
            return None
        return self.data_history[objectID][-1]

    def update(self, detections):
        if len(detections) == 0:
            for objectID in list(self.disappeared.keys()):
                self.disappeared[objectID] += 1
                if self.disappeared[objectID] > self.max_disappeared:
                    self.deregister(objectID)
            return {}

        boxes = [d['union_bbox'] for d in detections]

        if len(self.objects) == 0:
            for detection in detections:
                self.register(detection)
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
                    self.data_history[objectID].append(detections[j])
                    self.confidence_history[objectID].append(detections[j]['score'])
                    
                    # Update best violation data if score is higher
                    current_best_score = self.best_violation_data[objectID]['score']
                    if detections[j]['score'] > current_best_score:
                        self.best_violation_data[objectID].update(detections[j])
                    
                    matched_existing.add(i)
                    matched_new.add(j)
            
            for i, objectID in enumerate(objectIDs):
                if i not in matched_existing:
                    self.disappeared[objectID] += 1
                    if self.disappeared[objectID] > self.max_disappeared:
                        self.deregister(objectID)
            
            for j in range(len(boxes)):
                if j not in matched_new:
                    self.register(detections[j])
        
        results = {}
        for objectID in self.objects.keys():
            if self.disappeared.get(objectID, 0) == 0:
                stable_data = self.get_stable_data(objectID)
                if stable_data is not None:
                    results[objectID] = stable_data
        return results

class TripleRidingTracker:
    def __init__(self):
        self.tracker = StableTracker()
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
        if out.ndim < 2 or out.shape[1] < 4: 
            return [], []
            
        num_classes = out.shape[1] - 4
        if num_classes < 2: return [], []
            
        boxes = out[:, :4]
        scores = out[:, 4:4+num_classes].max(axis=1)
        classes = out[:, 4:4+num_classes].argmax(axis=1)
        
        mask = scores > conf_thresh
        if not np.any(mask): return [], []
        
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
            
            # Map back to ml_w, ml_h base
            x1 = (cx - bw/2) 
            y1 = (cy - bh/2) 
            x2 = (cx + bw/2) 
            y2 = (cy + bh/2) 
            
            raw_detections.append({
                'bbox': (float(max(0, x1)), float(max(0, y1)), float(min(ml_w, x2)), float(min(ml_h, y2))),
                'cid': int(f_classes[i]),
                'conf': float(f_scores[i])
            })
            
        motorcycles = [{'bbox': d['bbox'], 'score': d['conf'], 'area': get_box_area(d['bbox'])} 
                       for d in raw_detections if d['cid'] == MOTORCYCLE_CLASS_ID]
        motorcycles.sort(key=lambda x: x['area'], reverse=True)
        
        persons = [{'bbox': d['bbox'], 'score': d['conf'], 'used': False} 
                   for d in raw_detections if d['cid'] == PERSON_CLASS_ID]
        
        triple_riding_detections = detect_triple_riding_mra_hybrid(
            motorcycles, persons, ml_w, ml_h
        )
        
        tracked = self.tracker.update(triple_riding_detections)
        
        coords_payload = []
        alerts_payload = []
        
        for tid, d in tracked.items():
            coords_payload.append({
                "bbox": tuple(float(v) for v in d['union_bbox']),
                "riders": int(d['riders']),
                "conf": float(d['score']),
                "class_name": f"Triple Riding ({int(d['riders'])}P)",
                "tid": tid
            })
            
            # Prevent spamming alerts for the same track ID
            if tid not in self.saved_violations:
                self.saved_violations.add(tid)
                alerts_payload.append({
                    "type": "triple_riding",
                    "riders": d['riders'],
                    "tid": tid,
                    "score": d['score']
                })
        
        # Cleanup saved violations to avoid mem leak
        if len(self.saved_violations) > 1000:
             self.saved_violations.clear()
             
        return coords_payload, alerts_payload
