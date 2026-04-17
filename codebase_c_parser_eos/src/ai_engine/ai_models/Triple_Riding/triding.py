#!/usr/bin/env python3
"""
Triple Riding Detection with:
- Direct Display + File Output (non-RTSP mode)
- RTSP Streaming (RTSP mode)
- Stable Tracking (IoU-based with temporal smoothing)
- Violation Image Saving (highest confidence, no duplicates)
- FPS/Resolution/Frame overlay
- ALL FRAMES PROCESSED
"""

import os
import time
import threading
import numpy as np
import cairo
import cv2
from collections import OrderedDict, deque
from datetime import datetime

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import Gst, GLib, GstRtspServer

try:
    import tflite_runtime.interpreter as tflite
except ImportError:
    import tensorflow.lite as tflite

# ==============================================================================
# CONFIGURATION
# ==============================================================================
CAMERA = "/dev/video3"
WIDTH, HEIGHT = 1920, 1080
NMS_THRESHOLD = 0.45
CONF_THRESHOLD = 0.15

# Triple Riding Detection Classes
CLASS_NAMES = ["Person", "Motorcycle"]
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
MIN_STABLE_CONFIDENCE = 0.15

# Violation image saving
VIOLATION_OUTPUT_DIR = "Triple_ride_violation"

# RTSP
RTSP_PORT = "8554"
RTSP_MOUNT = "/triple"

# DEBUG MODE
DEBUG_MODE = False


# ==============================================================================
# DETECTION LOGIC (Preserved from original code)
# ==============================================================================
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
    """Calculate box area"""
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


def detect_triple_riding_mra_hybrid(motorcycles, persons, img_w, img_h, frame_count=0):
    """MRA HYBRID DETECTION - Core triple riding detection logic"""
    triple_riding_detections = []
    
    for person in persons:
        person['used'] = False
    
    if DEBUG_MODE and frame_count % 30 == 0:
        print(f"\n[DEBUG Frame {frame_count}]")
        print(f"  Total Motorcycles detected: {len(motorcycles)}")
        print(f"  Total Persons detected: {len(persons)}")
    
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
            violation_score = np.mean([mc_score] + person_scores)
            
        elif num_associated == 1:
            person = associated_persons[0]
            is_oversized, size_reason = is_oversized_person_box(person['bbox'], mc_bbox)
            
            if is_oversized:
                is_violation = True
                riders = 3
                reason = f"Oversized box: {size_reason}"
                violation_score = np.mean([mc_score, person['score']])
        
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


# ==============================================================================
# TRACKER (IoU-based with temporal smoothing)
# ==============================================================================
class StableTracker:
    """IoU-based tracker with temporal smoothing for stable prediction"""
    
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


# ==============================================================================
# MAIN DETECTOR CLASS
# ==============================================================================
class TripleRidingDetector:
    def __init__(self, model_path, video_src, ml_size=320, output_file=None, rtsp=False):
        self.video_src = video_src
        self.ml_size = ml_size
        self.output_file = output_file
        self.rtsp_enabled = rtsp
        self.lock = threading.Lock()
        self.tracked_objects = {}
        self.raw_detections = {'motorcycles': [], 'persons': []}
        self.frame_count = 0
        self.total_display_frames = 0
        self.fps = 0.0
        self.last_time = time.time()
        
        self.tracker = StableTracker()
        self.violation_dir = VIOLATION_OUTPUT_DIR
        os.makedirs(self.violation_dir, exist_ok=True)
        print(f"[info] Violation images will be saved to: {self.violation_dir}/")
        
        # Absolute best violation tracking
        self.absolute_best_data = {'score': 0.0, 'filepath': None}
        
        self.current_frame = None
        self.frame_lock = threading.Lock()
        
        # ML input size (for letterbox)
        self.ml_input_w = 640
        self.ml_input_h = 360
        
        self._load_model(model_path)
        
        if self.rtsp_enabled:
            self._build_rtsp_pipeline()
        else:
            self._build_display_pipeline()
        
    def _load_model(self, model_path):
        print(f"[info] Loading model: {model_path}")
        os.makedirs("/tmp/npu_cache", exist_ok=True)
        os.environ["VIV_VX_CACHE_BINARY_GRAPH_DIR"] = "/tmp/npu_cache"
        os.environ["VIV_VX_ENABLE_CACHE_GRAPH_BINARY"] = "1"
        
        if os.path.exists("/usr/lib/libvx_delegate.so"):
            delegate = tflite.load_delegate("/usr/lib/libvx_delegate.so")
            self.interpreter = tflite.Interpreter(model_path, experimental_delegates=[delegate])
            print("[info] NPU enabled")
        else:
            self.interpreter = tflite.Interpreter(model_path)
            print("[info] Using CPU backend")
        
        self.interpreter.allocate_tensors()
        self.input_details = self.interpreter.get_input_details()
        self.output_details = self.interpreter.get_output_details()
        self._dtype = self.input_details[0]['dtype']
        
        self.scale = None
        self.zp = 0
        try:
            qp = self.output_details[0].get('quantization_parameters', {})
            if qp.get('scales'): self.scale = float(qp['scales'][0])
            if qp.get('zero_points'): self.zp = int(qp['zero_points'][0])
        except: pass
        
        dummy = np.zeros((1, self.ml_size, self.ml_size, 3), dtype=self._dtype)
        self.interpreter.set_tensor(self.input_details[0]['index'], dummy)
        self.interpreter.invoke()
        print("[info] Model ready")

    def _build_rtsp_pipeline(self):
        """Build RTSP pipeline - full pipeline inside RTSP factory"""
        self.server = GstRtspServer.RTSPServer()
        self.server.set_service(RTSP_PORT)
        
        factory = GstRtspServer.RTSPMediaFactory()
        factory.set_shared(True)
        
        is_device = self.video_src.startswith("/dev/video")
        source = f"v4l2src device={self.video_src} ! video/x-raw,width={WIDTH},height={HEIGHT},framerate=30/1" if is_device else \
                 f"filesrc location={self.video_src} ! decodebin ! imxvideoconvert_g2d ! video/x-raw,width={WIDTH},height={HEIGHT}"

        # RTSP Pipeline with letterbox for ML
        launch = (
            f"( {source} ! "
            f"imxvideoconvert_g2d ! video/x-raw,format=BGRx ! "
            f"tee name=t "
            f"t. ! queue max-size-buffers=2 leaky=downstream ! cairooverlay name=overlay ! "
            f"vpuenc_h264 bitrate=5000 ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 "
            f"t. ! queue max-size-buffers=10 ! "
            f"imxvideoconvert_g2d ! video/x-raw,width={self.ml_input_w},height={self.ml_input_h},format=RGB16 ! "
            f"videoconvert ! video/x-raw,format=RGB ! appsink name=ml_sink emit-signals=true drop=false max-buffers=10 )"
        )
        
        factory.set_launch(launch)
        factory.connect("media-configure", self._on_media_configure)
        
        mounts = self.server.get_mount_points()
        mounts.add_factory(RTSP_MOUNT, factory)
        self.server.attach(None)
        print(f"[rtsp] Server ready at rtsp://0.0.0.0:{RTSP_PORT}{RTSP_MOUNT}")

    def _on_media_configure(self, factory, media):
        """Configure media when RTSP client connects"""
        element = media.get_element()
        if not element: return
        
        overlay = element.get_child_by_name("overlay")
        if overlay: 
            overlay.connect("draw", self._draw_overlay)
        
        ml_sink = element.get_child_by_name("ml_sink")
        if ml_sink: 
            ml_sink.connect("new-sample", self._on_ml_sample)

    def _build_display_pipeline(self):
        """Build display + file output pipeline (non-RTSP)"""
        is_device = self.video_src.startswith("/dev/video")
        
        if is_device:
            source = f"v4l2src device={self.video_src} ! video/x-raw,width={WIDTH},height={HEIGHT},framerate=30/1"
        else:
            source = f"filesrc location={self.video_src} ! decodebin ! imxvideoconvert_g2d ! video/x-raw,width={WIDTH},height={HEIGHT}"

        # File output (with videorate to normalize framerate for VPU encoder)
        file_output = ""
        if self.output_file:
            file_output = f"tee name=out_tee out_tee. ! queue ! videorate ! video/x-raw,framerate=30/1 ! vpuenc_h264 bitrate=8000 ! h264parse ! mp4mux ! filesink location={self.output_file} out_tee. ! queue ! "
        
        pipeline_str = (
            f"{source} ! "
            f"imxvideoconvert_g2d ! video/x-raw,format=BGRx ! "
            f"tee name=t "
            f"t. ! queue max-size-buffers=2 leaky=downstream ! cairooverlay name=overlay ! "
            f"{file_output}"
            f"autovideosink sync=false "
            f"t. ! queue max-size-buffers=10 ! "
            f"imxvideoconvert_g2d ! video/x-raw,width={self.ml_input_w},height={self.ml_input_h},format=RGB16 ! "
            f"videoconvert ! video/x-raw,format=RGB ! appsink name=ml_sink emit-signals=true drop=false max-buffers=10 sync=false"
        )
        
        print(f"[info] Pipeline: {pipeline_str}")
        self.pipeline = Gst.parse_launch(pipeline_str)
        
        overlay = self.pipeline.get_by_name("overlay")
        if overlay:
            overlay.connect("draw", self._draw_overlay)
            overlay.connect("caps-changed", self._on_caps_changed)
        
        ml_sink = self.pipeline.get_by_name("ml_sink")
        if ml_sink:
            ml_sink.connect("new-sample", self._on_ml_sample)
        
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_bus_message)
        
        print(f"[info] Display ready")
        if self.output_file:
            print(f"[info] Saving to: {self.output_file}")

    def _on_caps_changed(self, overlay, caps):
        structure = caps.get_structure(0)
        self.frame_width = structure.get_value("width")
        self.frame_height = structure.get_value("height")
        print(f"[info] Display resolution: {self.frame_width}x{self.frame_height}")

    def _on_bus_message(self, bus, message):
        t = message.type
        if t == Gst.MessageType.EOS:
            print("\n[info] End of stream!")
            self._print_summary()
            self.pipeline.set_state(Gst.State.NULL)
            self.loop.quit()
        elif t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            print(f"[error] {err}: {debug}")
            self.pipeline.set_state(Gst.State.NULL)
            self.loop.quit()

    def _on_ml_sample(self, appsink):
        self.frame_count += 1
        
        if self.frame_count % 10 == 0:
            now = time.time()
            self.fps = 10 / (now - self.last_time) if (now - self.last_time) > 0 else 0
            self.last_time = now
            
        sample = appsink.emit("pull-sample")
        if sample is None: return Gst.FlowReturn.OK
        
        buffer = sample.get_buffer()
        caps = sample.get_caps()
        ret, mem = buffer.map(Gst.MapFlags.READ)
        if not ret: return Gst.FlowReturn.OK
        
        h = caps.get_structure(0).get_value("height")
        w = caps.get_structure(0).get_value("width")
        frame = np.ndarray((h, w, 3), dtype=np.uint8, buffer=mem.data).copy()
        buffer.unmap(mem)
        
        with self.frame_lock:
            self.current_frame = frame.copy()
        
        # Run inference and get detections
        raw_detections = self._inference(frame)
        
        # Separate motorcycles and persons
        motorcycles = [{'bbox': d['bbox'], 'score': d['conf'], 'area': get_box_area(d['bbox'])} 
                       for d in raw_detections if d['cid'] == MOTORCYCLE_CLASS_ID]
        motorcycles.sort(key=lambda x: x['area'], reverse=True)
        
        persons = [{'bbox': d['bbox'], 'score': d['conf'], 'used': False} 
                   for d in raw_detections if d['cid'] == PERSON_CLASS_ID]
        
        # Detect triple riding violations
        triple_riding_detections = detect_triple_riding_mra_hybrid(
            motorcycles, persons, WIDTH, HEIGHT, self.frame_count
        )
        
        # Update tracker
        tracked = self.tracker.update(triple_riding_detections)
        
        # Save violation image if new best found
        self._save_absolute_best_violation_image(frame)
        
        with self.lock:
            self.tracked_objects = tracked
            self.raw_detections = {'motorcycles': motorcycles, 'persons': persons}
            
        if self.frame_count % 50 == 1 and not self.rtsp_enabled:
            viols = len(tracked)
            mcs = len(motorcycles)
            pers = len(persons)
            saved = 1 if self.absolute_best_data['filepath'] else 0
            print(f"[debug] Frame {self.frame_count}: {mcs} MCs, {pers} persons, {viols} violations, {saved} saved")
            
        return Gst.FlowReturn.OK

    def _letterbox(self, frame):
        """Fast letterbox with precomputed values for 640x360 -> 320x320"""
        new_w, new_h = 320, 180
        pad_y, pad_x = 70, 0
        scale = 0.5
        
        resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_NEAREST)
        
        if not hasattr(self, '_letterbox_buf'):
            self._letterbox_buf = np.full((320, 320, 3), 114, dtype=np.uint8)
        
        self._letterbox_buf[pad_y:pad_y+new_h, pad_x:pad_x+new_w] = resized
        
        return self._letterbox_buf, scale, pad_x, pad_y

    def _inference(self, frame):
        letterboxed, scale, pad_x, pad_y = self._letterbox(frame)
        
        if self._dtype == np.int8:
            input_tensor = (letterboxed.astype(np.int16) - 128).astype(np.int8).reshape(1, self.ml_size, self.ml_size, 3)
        else:
            input_tensor = letterboxed.reshape(1, self.ml_size, self.ml_size, 3)
            
        self.interpreter.set_tensor(self.input_details[0]['index'], input_tensor)
        self.interpreter.invoke()
        output = self.interpreter.get_tensor(self.output_details[0]['index'])
        
        out = output
        if self.scale: out = (out.astype(np.float32) - self.zp) * self.scale
        out = np.squeeze(out)
        if out.ndim == 2 and out.shape[0] < out.shape[1]: out = out.T
        if out.ndim < 2 or out.shape[1] < 4: return []
        
        # Get number of classes from output shape
        num_classes = out.shape[1] - 4
        if num_classes < 2: return []
            
        boxes = out[:, :4]
        scores = out[:, 4:4+num_classes].max(axis=1)
        classes = out[:, 4:4+num_classes].argmax(axis=1)
        
        mask = scores > CONF_THRESHOLD
        if not np.any(mask): return []
        
        f_boxes, f_scores, f_classes = boxes[mask], scores[mask], classes[mask]
        indices = self._nms(f_boxes, f_scores, NMS_THRESHOLD)
        
        results = []
        for i in indices:
            cx_norm, cy_norm, bw_norm, bh_norm = f_boxes[i]
            
            cx = cx_norm * self.ml_size - pad_x
            cy = cy_norm * self.ml_size - pad_y
            bw = bw_norm * self.ml_size
            bh = bh_norm * self.ml_size
            
            cx, cy = cx / scale, cy / scale
            bw, bh = bw / scale, bh / scale
            
            scale_x = WIDTH / self.ml_input_w
            scale_y = HEIGHT / self.ml_input_h
            
            x1 = (cx - bw/2) * scale_x
            y1 = (cy - bh/2) * scale_y
            x2 = (cx + bw/2) * scale_x
            y2 = (cy + bh/2) * scale_y
            
            results.append({
                'bbox': (int(max(0, x1)), int(max(0, y1)), int(min(WIDTH, x2)), int(min(HEIGHT, y2))),
                'cid': int(f_classes[i]),
                'conf': float(f_scores[i])
            })
        return results

    def _nms(self, boxes, scores, thresh):
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

    def _save_absolute_best_violation_image(self, frame):
        """Save the image with the absolute highest violation confidence"""
        best_data_overall = None
        max_score = 0.0
        
        for track_id, data in self.tracker.best_violation_data.items():
            if data['score'] > max_score:
                max_score = data['score']
                best_data_overall = data
                
        if best_data_overall is None or max_score <= self.absolute_best_data['score'] + 1e-4:
            return False
            
        union_bbox = best_data_overall['union_bbox']
        
        # Find the Track ID
        track_id_found = -1
        for tid, data in self.tracker.best_violation_data.items():
            if data is best_data_overall:
                track_id_found = tid
                break
        
        if track_id_found == -1:
            return False

        # Scale bbox to ML frame coordinates
        sx = self.ml_input_w / WIDTH
        sy = self.ml_input_h / HEIGHT
        
        x1, y1, x2, y2 = union_bbox
        px1 = max(0, int(x1 * sx))
        py1 = max(0, int(y1 * sy))
        px2 = min(self.ml_input_w, int(x2 * sx))
        py2 = min(self.ml_input_h, int(y2 * sy))
        
        if px2 <= px1 or py2 <= py1:
            return False
        
        crop = frame[py1:py2, px1:px2]
        if crop.size == 0:
            return False
        
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        filename = f"violation_ID{track_id_found}_conf{max_score:.3f}_riders{best_data_overall['riders']}_{timestamp}.png"
        filepath = os.path.join(self.violation_dir, filename)
        
        try:
            h, w = crop.shape[:2]
            stride = cairo.ImageSurface.format_stride_for_width(cairo.FORMAT_RGB24, w)
            bgra = np.zeros((h, stride // 4, 4), dtype=np.uint8)
            bgra[:, :w, 0] = crop[:, :, 2]
            bgra[:, :w, 1] = crop[:, :, 1]
            bgra[:, :w, 2] = crop[:, :, 0]
            surf = cairo.ImageSurface.create_for_data(bgra, cairo.FORMAT_RGB24, w, h, stride)
            surf.write_to_png(filepath)
            
            # Delete previous best
            previous_save_path = self.absolute_best_data['filepath']
            if previous_save_path and os.path.exists(previous_save_path):
                try: 
                    os.remove(previous_save_path)
                except: pass
            
            self.absolute_best_data['score'] = max_score
            self.absolute_best_data['filepath'] = filepath
            print(f"[SAVED] Violation conf={max_score:.3f} -> {filename}")
            return True
        except Exception as e:
            print(f"[ERROR] Save failed: {e}")
            return False

    def _draw_overlay(self, overlay, ctx, ts, dur):
        self.total_display_frames += 1
        with self.lock:
            tracked = self.tracked_objects.copy()
            raw = self.raw_detections.copy()
        
        mc_count = len(raw.get('motorcycles', []))
        violation_count = len(tracked)
        
        # Header
        ctx.set_source_rgba(0, 0, 0, 0.7)
        ctx.rectangle(0, 0, 550, 50)
        ctx.fill()
        ctx.set_source_rgb(0, 1, 0)
        ctx.set_font_size(20)
        ctx.move_to(15, 32)
        mode = "RTSP" if self.rtsp_enabled else "Display"
        ctx.show_text(f"{mode} | FPS: {self.fps:.1f} | Frame: {self.frame_count} | MCs: {mc_count}")
        
        # Violation count
        saved_count = 1 if self.absolute_best_data['filepath'] else 0
        
        ctx.set_source_rgba(0, 0, 0, 0.7)
        ctx.rectangle(WIDTH-300, 0, 300, 40)
        ctx.fill()
        ctx.set_source_rgb(1, 1, 1)
        ctx.set_font_size(18)
        ctx.move_to(WIDTH-290, 28)
        ctx.show_text(f"Violations: {violation_count} | Saved: {saved_count}")
        
        if violation_count > 0:
            # Warning banner
            ctx.set_source_rgba(0.8, 0, 0, 0.8)
            ctx.rectangle(0, 60, WIDTH, 40)
            ctx.fill()
            ctx.set_source_rgb(1, 1, 1)
            ctx.set_font_size(24)
            ctx.move_to(WIDTH/2 - 200, 88)
            ctx.show_text(f"⚠ TRIPLE RIDING VIOLATION DETECTED: {violation_count} ⚠")
        
        # Get ID of absolute best for highlighting
        absolute_best_id = -1
        if self.absolute_best_data['score'] > 0.0:
            for track_id, data in self.tracker.best_violation_data.items():
                if abs(data['score'] - self.absolute_best_data['score']) < 1e-4:
                    absolute_best_id = track_id
                    break
        
        # Draw tracked violations
        for tid, detection_data in tracked.items():
            union_bbox = detection_data['union_bbox']
            x1, y1, x2, y2 = union_bbox
            riders = detection_data['riders']
            score = detection_data['score']
            
            # Green if this is the best saved violation, else red
            if tid == absolute_best_id:
                ctx.set_source_rgb(0, 1, 0)
            else:
                ctx.set_source_rgb(1, 0, 0)
            
            ctx.set_line_width(4)
            ctx.rectangle(x1, y1, x2-x1, y2-y1)
            ctx.stroke()
            
            lbl = f"ID{tid}: {riders} Riders ({score:.2f})"
            ctx.set_source_rgba(0, 0, 0, 0.6)
            ctx.rectangle(x1, y1-30, len(lbl)*10+10, 30)
            ctx.fill()
            ctx.set_source_rgb(1, 1, 1)
            ctx.set_font_size(18)
            ctx.move_to(x1+5, y1-10)
            ctx.show_text(lbl)

    def _print_summary(self):
        print("\n" + "="*60)
        print("PROCESSING SUMMARY")
        print("="*60)
        print(f"Total ML frames: {self.frame_count}")
        print(f"Total display frames: {self.total_display_frames}")
        print(f"Violation images saved: {1 if self.absolute_best_data['filepath'] else 0}")
        print(f"Output dir: {os.path.abspath(self.violation_dir)}")
        if self.absolute_best_data['filepath']:
            print(f"\nBest violation saved:")
            print(f"  - {self.absolute_best_data['filepath']} (conf: {self.absolute_best_data['score']:.3f})")
        print("="*60)

    def run(self):
        self.loop = GLib.MainLoop()
        
        if self.rtsp_enabled:
            print("[info] Starting RTSP mode - waiting for client connection...")
            print(f"[info] Connect with: vlc rtsp://<board-ip>:{RTSP_PORT}{RTSP_MOUNT}")
        else:
            print("[info] Starting display pipeline...")
            self.pipeline.set_state(Gst.State.PLAYING)
        
        try:
            self.loop.run()
        except KeyboardInterrupt:
            print("\n[info] Stopped")
        finally:
            if not self.rtsp_enabled:
                self._print_summary()
                self.pipeline.set_state(Gst.State.NULL)
            print("[info] Done.")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description='Triple Riding Detection')
    parser.add_argument('-m', '--model', required=True, help='Path to TFLite model')
    parser.add_argument('-v', '--video', default=CAMERA, help='Video source (file or /dev/video)')
    parser.add_argument('-s', '--size', type=int, default=320, help='ML input size')
    parser.add_argument('-o', '--output', default=None, help='Output video file')
    parser.add_argument('-r', '--rtsp', action='store_true', help='Enable RTSP streaming')
    args = parser.parse_args()
    
    Gst.init(None)
    os.environ["XDG_RUNTIME_DIR"] = "/run/user/0"
    TripleRidingDetector(args.model, args.video, args.size, args.output, args.rtsp).run()

    
