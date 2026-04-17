
#!/usr/bin/env python3
"""
Helmet Detection with:
- Direct Display + File Output (non-RTSP mode)
- RTSP Streaming (RTSP mode)
- Stable Tracking (IoU-based with temporal smoothing)
- License Plate Saving for Violators ONLY (highest confidence, no duplicates)
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

# Config
CAMERA = "/dev/video3"
WIDTH, HEIGHT = 1920, 1080
NMS_THRESHOLD = 0.45
CONF_THRESHOLD = 0.20
CLASS_NAMES = ["With Helmet", "Without Helmet", "Licence"]

# Tracking parameters
SMOOTHING_FRAMES = 5
MAX_DISAPPEARED = 8
IOU_THRESHOLD = 0.4
MIN_STABLE_CONFIDENCE = 0.25

# License plate saving
PLATES_OUTPUT_DIR = "license_plates"

# RTSP
RTSP_PORT = "8554"
RTSP_MOUNT = "/helmet"


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
        avg_conf = np.mean([c for cls, c in zip(history, confidences) if cls == stable_class])
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
                    results[objectID] = {'bbox': self.objects[objectID], 'cid': stable_class, 'conf': avg_conf}
        return results


class HelmetDetector:
    def __init__(self, model_path, video_src, ml_size=320, output_file=None, rtsp=False):
        self.video_src = video_src
        self.ml_size = ml_size
        self.output_file = output_file
        self.rtsp_enabled = rtsp
        self.lock = threading.Lock()
        self.detections = {}
        self.raw_detections = []
        self.frame_count = 0
        self.total_display_frames = 0
        self.fps = 0.0
        self.last_time = time.time()
        
        self.tracker = StableTracker()
        self.saved_plates = {}
        self.plates_dir = PLATES_OUTPUT_DIR
        os.makedirs(self.plates_dir, exist_ok=True)
        print(f"[info] License plates will be saved to: {self.plates_dir}/")
        
        self.current_frame = None
        self.frame_lock = threading.Lock()
        
        # ML input size (for letterbox) - 640x360 is faster than direct 320x320!
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

        # File output
        file_output = ""
        if self.output_file:
            file_output = f"tee name=out_tee out_tee. ! queue ! vpuenc_h264 bitrate=8000 ! h264parse ! mp4mux ! filesink location={self.output_file} out_tee. ! queue ! "
        
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
        
        raw_detections = self._inference(frame)
        tracked = self.tracker.update(raw_detections)
        
        # Process license plates for violators
        self._process_violator_plates(tracked, raw_detections, frame)
        
        with self.lock:
            self.detections = tracked
            self.raw_detections = raw_detections
            
        if self.frame_count % 50 == 1 and not self.rtsp_enabled:
            plates = sum(1 for d in raw_detections if d['cid'] == 2)
            viols = sum(1 for o in tracked.values() if o['cid'] == 1)
            helms = sum(1 for o in tracked.values() if o['cid'] == 0)
            print(f"[debug] Frame {self.frame_count}: {len(raw_detections)} raw, {helms} helmet, {viols} violators, {plates} plates, {len(self.saved_plates)} saved")
            
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
        if out.ndim < 2 or out.shape[1] < 5: return []
            
        boxes = out[:, :4]
        scores = out[:, 4:7].max(axis=1)
        classes = out[:, 4:7].argmax(axis=1)
        
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

    def _get_plate_hash(self, plate_box, grid_size=150):
        x1, y1, x2, y2 = plate_box
        cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
        return f"{cx // grid_size}_{cy // grid_size}"

    def _is_plate_near_violator(self, plate_box, person_box):
        px1, py1, px2, py2 = person_box
        lx1, ly1, lx2, ly2 = plate_box
        
        pcx, pcy = (px1+px2)/2, (py1+py2)/2
        lcx, lcy = (lx1+lx2)/2, (ly1+ly2)/2
        pw, ph = px2 - px1, py2 - py1
        
        h_dist = abs(pcx - lcx)
        v_dist = lcy - py2
        
        if h_dist > pw * 3.0: return False
        if lcy < pcy * 0.8: return False
        if v_dist > ph * 4.0: return False
        
        return True

    def _process_violator_plates(self, tracked, raw_detections, frame):
        """Process plates - save only highest confidence, no duplicates"""
        plates = [(d['bbox'], d['conf']) for d in raw_detections if d['cid'] == 2]
        if not plates: return
        
        violators = [(tid, o['bbox']) for tid, o in tracked.items() if o['cid'] == 1]
        if not violators: return
        
        for plate_box, plate_conf in plates:
            for vid, vbox in violators:
                if self._is_plate_near_violator(plate_box, vbox):
                    h = self._get_plate_hash(plate_box)
                    
                    if h not in self.saved_plates or plate_conf > self.saved_plates[h]['conf']:
                        if h in self.saved_plates:
                            old = self.saved_plates[h].get('file')
                            if old and os.path.exists(old):
                                try: 
                                    os.remove(old)
                                except: pass
                        
                        fn = self._save_plate(plate_box, plate_conf, frame)
                        if fn:
                            self.saved_plates[h] = {'conf': plate_conf, 'file': fn}
                            print(f"[SAVED] Plate conf={plate_conf:.2f} -> {fn}")
                    break

    def _save_plate(self, plate_box, conf, frame):
        x1, y1, x2, y2 = plate_box
        
        sx = self.ml_input_w / WIDTH
        sy = self.ml_input_h / HEIGHT
        
        pad = 5
        px1 = max(0, int(x1 * sx) - pad)
        py1 = max(0, int(y1 * sy) - pad)
        px2 = min(self.ml_input_w, int(x2 * sx) + pad)
        py2 = min(self.ml_input_h, int(y2 * sy) + pad)
        
        if px2 <= px1 or py2 <= py1: return None
        
        crop = frame[py1:py2, px1:px2]
        if crop.size == 0: return None
        
        ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        fn = f"{self.plates_dir}/plate_{conf:.2f}_{ts}.png"
        
        try:
            h, w = crop.shape[:2]
            stride = cairo.ImageSurface.format_stride_for_width(cairo.FORMAT_RGB24, w)
            bgra = np.zeros((h, stride // 4, 4), dtype=np.uint8)
            bgra[:, :w, 0] = crop[:, :, 2]
            bgra[:, :w, 1] = crop[:, :, 1]
            bgra[:, :w, 2] = crop[:, :, 0]
            surf = cairo.ImageSurface.create_for_data(bgra, cairo.FORMAT_RGB24, w, h, stride)
            surf.write_to_png(fn)
            return fn
        except:
            return None

    def _draw_overlay(self, overlay, ctx, ts, dur):
        self.total_display_frames += 1
        with self.lock:
            tracked = self.detections.copy()
            raw = self.raw_detections.copy() if hasattr(self, 'raw_detections') else []
        
        # Header
        ctx.set_source_rgba(0, 0, 0, 0.7)
        ctx.rectangle(0, 0, 600, 50)
        ctx.fill()
        ctx.set_source_rgb(0, 1, 0)
        ctx.set_font_size(20)
        ctx.move_to(15, 32)
        mode = "RTSP" if self.rtsp_enabled else "Display"
        ctx.show_text(f"{mode} | FPS: {self.fps:.1f} | Frame: {self.frame_count} | Det: {len(tracked)}")
        
        # Counts
        plates_now = sum(1 for d in raw if d['cid'] == 2)
        viols = sum(1 for o in tracked.values() if o['cid'] == 1)
        
        ctx.set_source_rgba(0, 0, 0, 0.7)
        ctx.rectangle(WIDTH-280, 0, 280, 40)
        ctx.fill()
        ctx.set_source_rgb(1, 1, 1)
        ctx.set_font_size(18)
        ctx.move_to(WIDTH-270, 28)
        ctx.show_text(f"Plates: {plates_now} | Saved: {len(self.saved_plates)}")
        
        if viols > 0:
            ctx.set_source_rgba(0.8, 0, 0, 0.8)
            ctx.rectangle(WIDTH-280, 45, 280, 30)
            ctx.fill()
            ctx.set_source_rgb(1, 1, 1)
            ctx.move_to(WIDTH-270, 68)
            ctx.show_text(f"Violators: {viols}")
        
        # Draw tracked objects
        for tid, d in tracked.items():
            x1, y1, x2, y2 = d['bbox']
            cid, conf = d['cid'], d['conf']
            
            if cid == 0: ctx.set_source_rgb(0, 1, 0)
            elif cid == 1: ctx.set_source_rgb(1, 0, 0)
            else: ctx.set_source_rgb(1, 1, 0)
            
            ctx.set_line_width(4)
            ctx.rectangle(x1, y1, x2-x1, y2-y1)
            ctx.stroke()
            
            lbl = f"ID{tid}: {CLASS_NAMES[cid]} {conf:.2f}"
            ctx.set_source_rgba(0, 0, 0, 0.6)
            ctx.rectangle(x1, y1-30, len(lbl)*10+10, 30)
            ctx.fill()
            ctx.set_source_rgb(1, 1, 1)
            ctx.set_font_size(18)
            ctx.move_to(x1+5, y1-10)
            ctx.show_text(lbl)
        
        # Draw license plates (yellow)
        for d in raw:
            if d['cid'] == 2:
                x1, y1, x2, y2 = d['bbox']
                conf = d['conf']
                
                ctx.set_source_rgb(1, 1, 0)
                ctx.set_line_width(3)
                ctx.rectangle(x1, y1, x2-x1, y2-y1)
                ctx.stroke()
                
                lbl = f"Plate {conf:.2f}"
                ctx.set_source_rgba(1, 1, 0, 0.8)
                ctx.rectangle(x1, y1-25, len(lbl)*9+10, 25)
                ctx.fill()
                ctx.set_source_rgb(0, 0, 0)
                ctx.set_font_size(16)
                ctx.move_to(x1+5, y1-8)
                ctx.show_text(lbl)

    def _print_summary(self):
        print("\n" + "="*60)
        print("PROCESSING SUMMARY")
        print("="*60)
        print(f"Total ML frames: {self.frame_count}")
        print(f"Total display frames: {self.total_display_frames}")
        print(f"Unique plates saved: {len(self.saved_plates)}")
        print(f"Output dir: {os.path.abspath(self.plates_dir)}")
        if self.saved_plates:
            print("\nSaved plates (highest confidence only):")
            for h, info in self.saved_plates.items():
                print(f"  - {info['file']} (conf: {info['conf']:.2f})")
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
    parser = argparse.ArgumentParser()
    parser.add_argument('-m', '--model', required=True)
    parser.add_argument('-v', '--video', default=CAMERA)
    parser.add_argument('-s', '--size', type=int, default=320)
    parser.add_argument('-o', '--output', default=None, help='Output video file')
    parser.add_argument('-r', '--rtsp', action='store_true', help='Enable RTSP streaming')
    args = parser.parse_args()
    
    Gst.init(None)
    os.environ["XDG_RUNTIME_DIR"] = "/run/user/0"
    HelmetDetector(args.model, args.video, args.size, args.output, args.rtsp).run()
