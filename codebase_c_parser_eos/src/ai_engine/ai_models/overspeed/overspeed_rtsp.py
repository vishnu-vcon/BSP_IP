#!/usr/bin/env python3
"""
Overspeeding Detection with Cairo Overlay and RTSP/Display
===========================================================
Hardware: NXP i.MX8MP with NPU (libvx_delegate.so)

Core logic preserved from working overspeeding code:
- CPUPreprocessor with letterboxing
- OptimizedPostprocessor with vectorized NMS
- CentroidTracker for persistent IDs
- Speed calculation over 5 frames
- 12 vehicle classes (0-11)

Added:
- Cairo overlay (instead of cv2.putText/rectangle)
- GStreamer pipeline with tee for RTSP or Display output
- Optional file save
"""

import os
import time
import threading
import numpy as np
import cairo
import cv2
from collections import OrderedDict
from datetime import datetime

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import Gst, GLib, GstRtspServer

try:
    from tflite_runtime.interpreter import Interpreter
    from tflite_runtime.interpreter import load_delegate
    NPU_AVAILABLE = True
except ImportError:
    import tflite_runtime.interpreter as tflite
    NPU_AVAILABLE = False

# ============================================================================
# CONFIGURATION
# ============================================================================
CAMERA = "/dev/video3"
WIDTH, HEIGHT = 1920, 1080
ML_INPUT_SIZE = 320

# Detection parameters
CONF_THRESHOLD = 0.3
NMS_THRESHOLD = 0.45

# Speed detection
PIXELS_PER_METER = 25
SPEED_LIMIT_KMH = 60

# 12 vehicle classes (0-11) - IMPORTANT: This model uses multiple vehicle classes
VEHICLE_CLASS_IDS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]

# Tracking
MAX_DISAPPEARED = 15

# Output
PLATES_OUTPUT_DIR = "overspeed_plates"

# RTSP
RTSP_PORT = "8554"
RTSP_MOUNT = "/overspeed"


# ============================================================================
# CPU PREPROCESSOR (from working code)
# ============================================================================
class CPUPreprocessor:
    """Fast CPU-based preprocessing with pre-allocated buffers"""
    def __init__(self, target_width, target_height, input_dtype):
        self.target_width = target_width
        self.target_height = target_height
        self.input_dtype = input_dtype
        self.output_buffer = np.zeros((1, target_height, target_width, 3), dtype=input_dtype)
        
    def process(self, frame):
        """Process frame - returns (tensor, scale, pad_x, pad_y)"""
        orig_h, orig_w = frame.shape[:2]
        target_h, target_w = self.target_height, self.target_width
        
        scale = min(target_w / orig_w, target_h / orig_h)
        new_w = int(orig_w * scale)
        new_h = int(orig_h * scale)
        pad_x = (target_w - new_w) // 2
        pad_y = (target_h - new_h) // 2
        
        if new_w != orig_w or new_h != orig_h:
            frame = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
        
        padded = np.full((target_h, target_w, 3), 114, dtype=np.uint8)
        padded[pad_y:pad_y+new_h, pad_x:pad_x+new_w] = frame
        
        if self.input_dtype == np.uint8:
            np.copyto(self.output_buffer[0], padded)
        elif self.input_dtype == np.int8:
            np.subtract(padded, 128, out=self.output_buffer[0], casting='unsafe')
        else:
            self.output_buffer[0] = padded.astype(np.float32) / 255.0
            
        return self.output_buffer, scale, pad_x, pad_y


# ============================================================================
# OPTIMIZED POSTPROCESSOR (from working code)
# ============================================================================
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
        if scale_params['scale'] is not None:
            out = (out.astype(np.float32) - scale_params['zero_point']) * scale_params['scale']
        
        if len(out.shape) == 3:
            out = np.squeeze(out).T
        else:
            out = np.squeeze(out)
        
        if out.ndim == 1:
            out = np.expand_dims(out, 0)
        if out.shape[0] == 0 or out.shape[1] < 5:
            return [], [], []
        
        frame_h, frame_w = frame_shape
        in_h, in_w = input_shape
        
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
        
        return boxes[keep].astype(np.int32).tolist(), sel_scores[keep].tolist(), sel_cls[keep].tolist()


# ============================================================================
# CENTROID TRACKER (from working code)
# ============================================================================
class CentroidTracker:
    def __init__(self, maxDisappeared=50):
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


# ============================================================================
# MAIN DETECTOR WITH CAIRO OVERLAY AND GSTREAMER
# ============================================================================
class OverspeedDetector:
    def __init__(self, model_path, video_src, output_file=None, rtsp_mode=False):
        self.video_src = video_src
        self.output_file = output_file
        self.rtsp_mode = rtsp_mode
        self.is_camera = video_src.startswith("/dev/video")
        
        self.lock = threading.Lock()
        self.frame_count = 0
        self.fps = 0.0
        self.actual_fps = 30.0  # Will be updated
        self.last_fps_time = time.time()
        
        # ML input dimensions
        self.ml_input_w, self.ml_input_h = 640, 360
        
        # State for drawing
        self.vehicle_boxes = []
        self.vehicle_scores = []
        self.vehicle_classes = []
        self.tracked_objects = {}
        self.track_history = {}
        self.speeds = {}
        self.saved_violations = set()
        self.total_violations = 0
        
        # Output directory
        self.plates_dir = PLATES_OUTPUT_DIR
        os.makedirs(self.plates_dir, exist_ok=True)
        print(f"[INFO] Violations dir: {self.plates_dir}/")
        
        # Load model
        self._load_model(model_path)
        
        # Initialize tracker
        self.tracker = CentroidTracker(maxDisappeared=MAX_DISAPPEARED)
        
        # Build pipeline
        if self.rtsp_mode:
            self._build_rtsp_pipeline()
        else:
            self._build_display_pipeline()
    
    def _load_model(self, model_path):
        print(f"[INFO] Loading: {model_path}")
        os.makedirs("/tmp/npu_cache", exist_ok=True)
        os.environ["VIV_VX_CACHE_BINARY_GRAPH_DIR"] = "/tmp/npu_cache"
        os.environ["VIV_VX_ENABLE_CACHE_GRAPH_BINARY"] = "1"
        
        if NPU_AVAILABLE:
            try:
                delegate = load_delegate('libvx_delegate.so')
                self.interpreter = Interpreter(model_path, experimental_delegates=[delegate])
                print("[INFO] NPU enabled")
            except Exception as e:
                print(f"[WARN] NPU failed: {e}, using CPU")
                self.interpreter = Interpreter(model_path, num_threads=4)
        else:
            self.interpreter = tflite.Interpreter(model_path, num_threads=4)
            print("[INFO] Using CPU")
        
        self.interpreter.allocate_tensors()
        self.input_details = self.interpreter.get_input_details()
        self.output_details = self.interpreter.get_output_details()
        
        input_shape = self.input_details[0]['shape']
        in_h, in_w = int(input_shape[1]), int(input_shape[2])
        in_dtype = self.input_details[0]['dtype']
        
        self.in_h, self.in_w = in_h, in_w
        
        # Get quantization parameters
        self.scale_params = {'scale': None, 'zero_point': 0}
        try:
            qp = self.output_details[0].get('quantization_parameters', {})
            if qp.get('scales'):
                self.scale_params['scale'] = float(qp['scales'][0])
            if qp.get('zero_points'):
                self.scale_params['zero_point'] = int(qp['zero_points'][0])
        except:
            pass
        
        # Initialize preprocessor and postprocessor
        self.preprocessor = CPUPreprocessor(in_w, in_h, in_dtype)
        self.postprocessor = OptimizedPostprocessor(
            conf_threshold=CONF_THRESHOLD,
            nms_threshold=NMS_THRESHOLD,
            topk=500,
            vehicle_class_ids=VEHICLE_CLASS_IDS
        )
        
        # Warmup
        dummy = np.zeros((480, 640, 3), dtype=np.uint8)
        dummy_input, _, _, _ = self.preprocessor.process(dummy)
        self.interpreter.set_tensor(self.input_details[0]['index'], dummy_input)
        self.interpreter.invoke()
        print("[INFO] Model ready")

    def _build_display_pipeline(self):
        if self.is_camera:
            source = f"v4l2src device={self.video_src} ! video/x-raw,width={WIDTH},height={HEIGHT},framerate=30/1"
        else:
            source = f"filesrc location={self.video_src} ! qtdemux ! h264parse ! vpudec ! imxvideoconvert_g2d ! video/x-raw,width={WIDTH},height={HEIGHT}"
        
        if self.output_file:
            pipeline_str = (
                f"{source} ! "
                f"imxvideoconvert_g2d ! video/x-raw,format=BGRx ! "
                f"tee name=t "
                f"t. ! queue max-size-buffers=2 leaky=downstream ! cairooverlay name=overlay ! "
                f"tee name=out "
                f"out. ! queue ! vpuenc_h264 bitrate=8000 ! h264parse ! mp4mux ! filesink location={self.output_file} "
                f"out. ! queue ! autovideosink sync=false "
                f"t. ! queue max-size-buffers=10 ! "
                f"imxvideoconvert_g2d ! video/x-raw,width={self.ml_input_w},height={self.ml_input_h},format=RGB16 ! "
                f"videoconvert ! video/x-raw,format=RGB ! appsink name=ml_sink emit-signals=true drop=false max-buffers=10 sync=false"
            )
        else:
            pipeline_str = (
                f"{source} ! "
                f"imxvideoconvert_g2d ! video/x-raw,format=BGRx ! "
                f"tee name=t "
                f"t. ! queue max-size-buffers=2 leaky=downstream ! cairooverlay name=overlay ! autovideosink sync=false "
                f"t. ! queue max-size-buffers=10 ! "
                f"imxvideoconvert_g2d ! video/x-raw,width={self.ml_input_w},height={self.ml_input_h},format=RGB16 ! "
                f"videoconvert ! video/x-raw,format=RGB ! appsink name=ml_sink emit-signals=true drop=false max-buffers=10 sync=false"
            )
        
        print(f"[INFO] Mode: DISPLAY" + (" + SAVE" if self.output_file else ""))
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

    def _build_rtsp_pipeline(self):
        self.server = GstRtspServer.RTSPServer()
        self.server.set_service(RTSP_PORT)
        
        factory = GstRtspServer.RTSPMediaFactory()
        factory.set_shared(True)
        
        if self.is_camera:
            source = f"v4l2src device={self.video_src} ! video/x-raw,width={WIDTH},height={HEIGHT},framerate=30/1"
        else:
            source = f"filesrc location={self.video_src} ! qtdemux ! h264parse ! vpudec ! imxvideoconvert_g2d ! video/x-raw,width={WIDTH},height={HEIGHT}"
        
        if self.output_file:
            launch = (
                f"( {source} ! "
                f"imxvideoconvert_g2d ! video/x-raw,format=BGRx ! "
                f"tee name=t "
                f"t. ! queue max-size-buffers=2 leaky=downstream ! cairooverlay name=overlay ! "
                f"tee name=out "
                f"out. ! queue ! vpuenc_h264 bitrate=8000 ! h264parse ! mp4mux ! filesink location={self.output_file} "
                f"out. ! queue ! vpuenc_h264 bitrate=5000 ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 "
                f"t. ! queue max-size-buffers=10 ! "
                f"imxvideoconvert_g2d ! video/x-raw,width={self.ml_input_w},height={self.ml_input_h},format=RGB16 ! "
                f"videoconvert ! video/x-raw,format=RGB ! appsink name=ml_sink emit-signals=true drop=false max-buffers=10 )"
            )
        else:
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
        
        print(f"[INFO] Mode: RTSP" + (" + SAVE" if self.output_file else ""))
        print(f"[RTSP] rtsp://0.0.0.0:{RTSP_PORT}{RTSP_MOUNT}")

    def _on_media_configure(self, factory, media):
        element = media.get_element()
        if element:
            overlay = element.get_child_by_name("overlay")
            if overlay:
                overlay.connect("draw", self._draw_overlay)
            ml_sink = element.get_child_by_name("ml_sink")
            if ml_sink:
                ml_sink.connect("new-sample", self._on_ml_sample)
            print("[RTSP] Client connected")

    def _on_caps_changed(self, overlay, caps):
        s = caps.get_structure(0)
        print(f"[INFO] Resolution: {s.get_value('width')}x{s.get_value('height')}")

    def _on_bus_message(self, bus, message):
        t = message.type
        if t == Gst.MessageType.EOS:
            print("\n[INFO] End of stream")
            self._print_summary()
            self.pipeline.set_state(Gst.State.NULL)
            self.loop.quit()
        elif t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            print(f"[ERROR] {err}: {debug}")
            self.pipeline.set_state(Gst.State.NULL)
            self.loop.quit()

    def _on_ml_sample(self, appsink):
        self.frame_count += 1
        
        if self.frame_count % 10 == 0:
            now = time.time()
            self.fps = 10 / (now - self.last_fps_time) if (now - self.last_fps_time) > 0 else 0
            self.actual_fps = self.fps if self.fps > 0 else 30.0
            self.last_fps_time = now
        
        sample = appsink.emit("pull-sample")
        if not sample:
            return Gst.FlowReturn.OK
        
        buf = sample.get_buffer()
        caps = sample.get_caps()
        ret, mem = buf.map(Gst.MapFlags.READ)
        if not ret:
            return Gst.FlowReturn.OK
        
        s = caps.get_structure(0)
        h, w = s.get_value("height"), s.get_value("width")
        frame = np.ndarray((h, w, 3), dtype=np.uint8, buffer=mem.data).copy()
        buf.unmap(mem)
        
        # Run inference
        input_tensor, scale, pad_x, pad_y = self.preprocessor.process(frame)
        self.interpreter.set_tensor(self.input_details[0]['index'], input_tensor)
        self.interpreter.invoke()
        output_data = self.interpreter.get_tensor(self.output_details[0]['index'])
        
        # Post-process (output in ML frame coordinates: 640x360)
        vehicle_boxes, vehicle_scores, vehicle_classes = self.postprocessor.process(
            output_data, (self.ml_input_h, self.ml_input_w), (self.in_h, self.in_w),
            self.scale_params, scale, pad_x, pad_y
        )
        
        # Track objects
        tracked_objects = self.tracker.update(vehicle_boxes)
        
        # Calculate speeds
        speeds = {}
        for (objectID, centroid) in tracked_objects.items():
            if objectID not in self.track_history:
                self.track_history[objectID] = []
            self.track_history[objectID].append(centroid)
            
            speed_kmh = 0
            if len(self.track_history[objectID]) > 5:
                pixel_dist = np.linalg.norm(
                    np.array(self.track_history[objectID][-1]) -
                    np.array(self.track_history[objectID][-5])
                )
                meter_dist = pixel_dist / PIXELS_PER_METER
                time_sec = 4.0 / float(self.actual_fps)
                if time_sec > 0:
                    speed_mps = meter_dist / time_sec
                    speed_kmh = speed_mps * 3.6
            
            speeds[objectID] = speed_kmh
            
            # Check violation
            if speed_kmh > SPEED_LIMIT_KMH and objectID not in self.saved_violations:
                self.tracker.mark_violation(objectID)
                
                # Find corresponding box and save
                for i, box in enumerate(vehicle_boxes):
                    x1, y1, x2, y2 = box
                    box_centroid = ((x1 + x2) // 2, (y1 + y2) // 2)
                    dist = np.linalg.norm(np.array(box_centroid) - np.array(centroid))
                    if dist < 50:
                        self._save_violation(frame, box, objectID, speed_kmh)
                        self.saved_violations.add(objectID)
                        self.total_violations += 1
                        print(f"[VIOLATION] ID{objectID} @ {int(speed_kmh)}km/h")
                        break
        
        # Update shared state for drawing
        with self.lock:
            self.vehicle_boxes = vehicle_boxes
            self.vehicle_scores = vehicle_scores
            self.vehicle_classes = vehicle_classes
            self.tracked_objects = tracked_objects
            self.speeds = speeds
        
        if self.frame_count % 30 == 1:
            print(f"[{self.frame_count}] det:{len(vehicle_boxes)} trk:{len(tracked_objects)} viol:{self.total_violations} fps:{self.fps:.1f}")
        
        return Gst.FlowReturn.OK

    def _save_violation(self, frame, box, object_id, speed):
        try:
            x1, y1, x2, y2 = box
            # Extract plate region (bottom 40% of vehicle)
            box_height = y2 - y1
            plate_y1 = int(y1 + box_height * 0.6)
            
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            
            # Save plate region
            plate_region = frame[plate_y1:y2, x1:x2]
            if plate_region.size > 0:
                # Scale to original resolution
                sx = self.ml_input_w / WIDTH
                sy = self.ml_input_h / HEIGHT
                px1, py1 = int(x1 * sx), int(plate_y1 * sy)
                px2, py2 = int(x2 * sx), int(y2 * sy)
                px1, py1 = max(0, px1), max(0, py1)
                px2, py2 = min(self.ml_input_w, px2), min(self.ml_input_h, py2)
                
                plate_fn = f"{self.plates_dir}/plate_ID{object_id}_{int(speed)}kmh_{timestamp}.png"
                # Use Cairo to save
                crop = frame[py1:py2, px1:px2]
                if crop.size > 0:
                    cv2.imwrite(plate_fn, cv2.cvtColor(crop, cv2.COLOR_RGB2BGR))
            
            # Save full vehicle
            vx1, vy1 = int(x1 * sx), int(y1 * sy)
            vx2, vy2 = int(x2 * sx), int(y2 * sy)
            vx1, vy1 = max(0, vx1), max(0, vy1)
            vx2, vy2 = min(self.ml_input_w, vx2), min(self.ml_input_h, vy2)
            
            vehicle_fn = f"{self.plates_dir}/vehicle_ID{object_id}_{int(speed)}kmh_{timestamp}.png"
            vcrop = frame[vy1:vy2, vx1:vx2]
            if vcrop.size > 0:
                cv2.imwrite(vehicle_fn, cv2.cvtColor(vcrop, cv2.COLOR_RGB2BGR))
                
        except Exception as e:
            print(f"[ERROR] Save: {e}")

    def _draw_overlay(self, overlay, ctx, ts, dur):
        """Cairo overlay drawing - scale from ML coords to display coords"""
        # Scale factors: ML (640x360) -> Display (1920x1080)
        scale_x = WIDTH / self.ml_input_w   # 1920/640 = 3.0
        scale_y = HEIGHT / self.ml_input_h  # 1080/360 = 3.0
        
        with self.lock:
            boxes = self.vehicle_boxes.copy()
            tracked = self.tracked_objects.copy()
            speeds = self.speeds.copy()
        
        num_tracked = len(tracked)
        num_over = sum(1 for s in speeds.values() if s > SPEED_LIMIT_KMH)
        
        # Header bar
        ctx.set_source_rgba(0, 0, 0, 0.7)
        ctx.rectangle(0, 0, 600, 75)
        ctx.fill()
        
        src = "CAM" if self.is_camera else "VIDEO"
        mode = "RTSP" if self.rtsp_mode else "DISPLAY"
        
        ctx.set_source_rgb(0, 1, 0)
        ctx.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        ctx.set_font_size(18)
        ctx.move_to(10, 25)
        ctx.show_text(f"{src}|{mode} Frame:{self.frame_count} Det:{len(boxes)} Trk:{num_tracked} Viol:{self.total_violations}")
        
        ctx.set_source_rgb(0, 1, 1)
        ctx.set_font_size(16)
        ctx.move_to(10, 48)
        ctx.show_text(f"FPS: {self.fps:.1f}")
        
        ctx.set_source_rgb(1, 1, 1)
        ctx.move_to(10, 68)
        ctx.show_text(f"Limit: {SPEED_LIMIT_KMH} km/h | PIXELS_PER_METER: {PIXELS_PER_METER}")
        
        # Stats (top right)
        ctx.set_source_rgba(0, 0, 0, 0.7)
        ctx.rectangle(WIDTH - 200, 0, 200, 35)
        ctx.fill()
        ctx.set_source_rgb(1, 1, 1)
        ctx.set_font_size(16)
        ctx.move_to(WIDTH - 190, 24)
        ctx.show_text(f"Saved: {len(self.saved_violations)}")
        
        # Violation alert
        if num_over > 0:
            ctx.set_source_rgba(0.9, 0, 0, 0.8)
            ctx.rectangle(WIDTH - 200, 40, 200, 28)
            ctx.fill()
            ctx.set_source_rgb(1, 1, 1)
            ctx.set_font_size(15)
            ctx.move_to(WIDTH - 190, 60)
            ctx.show_text(f"OVERSPEEDING: {num_over}")
        
        # Draw detection boxes (green) - scaled to display
        for i, box in enumerate(boxes):
            x1, y1, x2, y2 = box
            # Scale to display resolution
            dx1, dy1 = int(x1 * scale_x), int(y1 * scale_y)
            dx2, dy2 = int(x2 * scale_x), int(y2 * scale_y)
            ctx.set_source_rgb(0, 1, 0)
            ctx.set_line_width(2)
            ctx.rectangle(dx1, dy1, dx2-dx1, dy2-dy1)
            ctx.stroke()
        
        # Draw tracked objects with speed - scaled to display
        for (objectID, centroid) in tracked.items():
            speed = speeds.get(objectID, 0)
            is_over = speed > SPEED_LIMIT_KMH
            is_violating = self.tracker.is_violating(objectID)
            
            # Scale centroid to display resolution
            cx = int(centroid[0] * scale_x)
            cy = int(centroid[1] * scale_y)
            
            # Color: Red if overspeeding, Yellow otherwise
            if is_over or is_violating:
                ctx.set_source_rgb(1, 0, 0)
            else:
                ctx.set_source_rgb(1, 1, 0)
            
            # Draw centroid
            ctx.arc(cx, cy, 8, 0, 2 * 3.14159)
            ctx.fill()
            
            # Label
            label = f"ID{objectID}: {int(speed)}km/h"
            ctx.set_source_rgba(0, 0, 0, 0.7)
            ctx.rectangle(cx - 50, cy - 35, len(label) * 10 + 12, 26)
            ctx.fill()
            
            if is_over or is_violating:
                ctx.set_source_rgb(1, 0.3, 0.3)
            else:
                ctx.set_source_rgb(1, 1, 0.3)
            
            ctx.set_font_size(16)
            ctx.move_to(cx - 45, cy - 15)
            ctx.show_text(label)

    def _print_summary(self):
        print("\n" + "="*50)
        print(f"Frames: {self.frame_count} | Violations: {self.total_violations}")
        print(f"Saved to: {self.plates_dir}/")
        print("="*50)

    def run(self):
        self.loop = GLib.MainLoop()
        print(f"\n[INFO] Input: {'Camera' if self.is_camera else 'Video'} {self.video_src}")
        print(f"[INFO] Output: {'RTSP' if self.rtsp_mode else 'Display'}" + (f" + {self.output_file}" if self.output_file else ""))
        print(f"[INFO] Speed limit: {SPEED_LIMIT_KMH} km/h, PIXELS_PER_METER: {PIXELS_PER_METER}\n")
        
        if self.rtsp_mode:
            print("[INFO] Waiting for RTSP client...")
        else:
            print("[INFO] Starting pipeline...")
            self.pipeline.set_state(Gst.State.PLAYING)
        
        try:
            self.loop.run()
        except KeyboardInterrupt:
            print("\n[INFO] Stopped")
        finally:
            if not self.rtsp_mode:
                self._print_summary()
                self.pipeline.set_state(Gst.State.NULL)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('-m', '--model', required=True)
    parser.add_argument('-v', '--video', default=CAMERA)
    parser.add_argument('-o', '--output', default=None)
    parser.add_argument('-r', '--rtsp', action='store_true')
    args = parser.parse_args()
    
    Gst.init(None)
    os.environ["XDG_RUNTIME_DIR"] = "/run/user/0"
    
    OverspeedDetector(args.model, args.video, args.output, args.rtsp).run()
