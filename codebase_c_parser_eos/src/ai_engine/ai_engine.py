#!/usr/bin/env python3
"""
camera-ai.service
------------------------
AI Inference Data Plane.
Listens on D-Bus. Reads Shared Memory. Writes coordinates and alerts to ZeroMQ.
Supported models: helmet, triple_riding, overspeed.
"""

import time
import threading
import logging
import sys
import os
import json
import numpy as np
import cv2

import gi
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
SHM_DIR = "/tmp/gst_shm"

gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib

try:
    import tflite_runtime.interpreter as tflite
except ImportError:
    import tensorflow.lite as tflite

# Import shared modules
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common.event_broker import MQPublisher, TOPIC_AI_COORDS, TOPIC_ALERTS

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)8s] [%(name)s] %(message)s"
)
log = logging.getLogger("ai_engine")

# ══════════════════════════════════════════════════════════════════════
#  D-Bus Interface
# ══════════════════════════════════════════════════════════════════════
class AIEngineDBus(dbus.service.Object):
    def __init__(self, engine):
        self.engine = engine
        bus_name = dbus.service.BusName('com.camera.AIEngine', bus=dbus.SessionBus(), replace_existing=True)
        dbus.service.Object.__init__(self, bus_name, '/com/camera/AIEngine')
        log.debug("D-Bus interface registered at /com/camera/AIEngine")

    @dbus.service.method('com.camera.AIEngine', in_signature='s', out_signature='s')
    def LoadModel(self, params_json):
        log.debug("DBUS: [LoadModel] Received: %s", params_json)
        params = json.loads(params_json)
        res = self.engine.load_model(params)
        return json.dumps(res)

    @dbus.service.method('com.camera.AIEngine', in_signature='s', out_signature='s')
    def SetAIConfig(self, config_json):
        log.debug("DBUS: [SetAIConfig] Received: %s", config_json)
        config = json.loads(config_json)
        res = self.engine.set_ai_config(config)
        return json.dumps(res)

    @dbus.service.method('com.camera.AIEngine', in_signature='s', out_signature='s')
    def StopModel(self, params_json):
        log.debug("DBUS: [StopModel] Received: %s", params_json)
        params = json.loads(params_json)
        res = self.engine.stop_model(params)
        return json.dumps(res)

    @dbus.service.method('com.camera.AIEngine', in_signature='s', out_signature='s')
    def SetLogLevel(self, level):
        log.debug("DBUS: [SetLogLevel] Level: %s", level)
        res = self.engine.set_log_level(level)
        return json.dumps(res)

    @dbus.service.method('com.camera.AIEngine', in_signature='', out_signature='s')
    def GetStatus(self):
        res = self.engine.get_status()
        return json.dumps(res)

    @dbus.service.method('com.camera.AIEngine', in_signature='s', out_signature='s')
    def DumpDOT(self, timestamp):
        log.debug("DBUS: [DumpDOT] Timestamp: %s", timestamp)
        res = self.engine.dump_dot(timestamp)
        return json.dumps(res)


# ══════════════════════════════════════════════════════════════════════
#  AI Engine (Manager & Inference)
# ══════════════════════════════════════════════════════════════════════
class RealAIEngine:
    def __init__(self):
        log.debug("Booting AI Engine...")
        Gst.init(None)

        self.publisher = MQPublisher(port=5555)
        self._last_coord_publish = 0
        self._HEARTBEAT_INTERVAL = 2.0  # seconds, only for empty frames
        self.active_pipeline = None
        self.interpreter = None
        
        self.current_model = None
        self.current_lens = None
        self.tracker = None
        self._model_lock = threading.Lock()  # Serializes interpreter access vs model swap
        
        # ML Input resolution (expected by the models)
        self.ml_input_w = 640
        self.ml_input_h = 360
        self.ml_size = 320 # Inner size for tflite
        self._dtype = None
        self.scale = None
        self.zp = 0
        self.input_details = None
        self.output_details = None

        # AI Configuration
        self.ai_snapshots_enabled = True
        self.ai_threshold = 0.4
        self._frame_count = 0

    # --- Pipeline & Model Management ---

    def load_model(self, params: dict) -> dict:
        lens = params.get("lens")
        if not lens or lens not in ["lens1", "lens2"]:
            log.warning("No valid lens provided for model load, defaulting to 'lens1'")
            lens = "lens1"
            
        model_name = params.get("model")
        
        if model_name not in ["helmet", "triple_riding", "overspeed", "person"]:
            return {"status": "error", "message": f"Unsupported model: {model_name}"}
 
        if self.current_lens and self.current_lens != lens:
            self.stop_model({})

        # If pipeline already exists for THIS lens, we don't need to rebuild it! (B15 Fix)
        if self.active_pipeline and self.current_lens == lens:
            log.info("Pipeline already exists for this lens. Skipping model change logic to avoid stale state.")
            return {"status": "loading", "model": model_name, "lens": lens}

        log.debug("Loading AI [%s] for hardware [%s]...", model_name, lens)
        
        # AI Configuration
        self.ai_snapshots_enabled = True
        self.ai_threshold = 0.4
        self.snapshot_threshold = 0.5 # Higher threshold for recordings/snapshots
        self._frame_count = 0
        self._last_log_time = time.time()
        
        # Load TFLite Model
        base_dir = os.path.dirname(os.path.abspath(__file__))
        model_path = ""
        
        if model_name == "helmet":
            model_path = os.path.join(base_dir, "ai_models/Helmet_Detection/saved_model_converted_quantized_320.tflite")
            from ai_utils_helmet import HelmetTracker
            self.tracker = HelmetTracker()
        elif model_name == "triple_riding":
            model_path = os.path.join(base_dir, "ai_models/Triple_Riding/saved_model_triding_320.tflite")
            from ai_utils_triding import TripleRidingTracker
            self.tracker = TripleRidingTracker()
        elif model_name == "overspeed":
            model_path = os.path.join(base_dir, "ai_models/overspeed/model_calibrated_int8_rog.tflite")
            from ai_utils_overspeed import OverspeedTracker
            self.tracker = OverspeedTracker()
        elif model_name == "person":
            # Using triple riding model as it clearly handles Class 0 as Person
            model_path = os.path.join(base_dir, "ai_models/Triple_Riding/saved_model_triding_320.tflite")
            from ai_utils_person import PersonTracker
            self.tracker = PersonTracker()

        model_path = os.path.abspath(model_path)
        if not os.path.exists(model_path):
            return {"status": "error", "message": f"Model file not found: {model_path}"}
            
        # Pause inference while we swap the interpreter
        with self._model_lock:
            if not self._init_tflite(model_path):
                 return {"status": "error", "message": "Failed to initialize TFLite"}

        self.current_model = model_name
        self.current_lens = lens
        socket_path = f"{SHM_DIR}/{lens}_raw.sock"
        
        # Publish loading state so Video Engine clears stale boxes
        self.publisher.publish(TOPIC_AI_COORDS, {"lens": lens, "model": model_name, "coords": [], "ml_w": self.ml_input_w, "ml_h": self.ml_input_h})

        # Ask C engine to attach SHM (non-fatal if engine not ready)
        self._request_shm_attach(lens, socket_path)

        # Build pipeline from Shared Memory (NV12 format — matches capture_tee directly)
        # B17 Update: Use imxvideoconvert_g2d for hardware-accelerated scaling and format conversion
        pipeline_str = (
            f"shmsrc socket-path={socket_path} do-timestamp=true is-live=true ! "
            f"video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! "
            f"queue max-size-buffers=1 leaky=downstream ! "
            f"imxvideoconvert_g2d ! video/x-raw,width={self.ml_input_w},height={self.ml_input_h},format=BGRx ! "
            f"videoconvert ! video/x-raw,format=BGR ! "
            f"appsink name=ml_sink emit-signals=true drop=true max-buffers=1 sync=false"
        )
        
        log.debug("AI GST Pipeline: %s", pipeline_str)
        self.active_pipeline = Gst.parse_launch(pipeline_str)
        
        appsink = self.active_pipeline.get_by_name("ml_sink")
        appsink.connect("new-sample", self._on_new_sample)
        
        self.active_pipeline.set_state(Gst.State.READY) # Get it ready
        
        # Start Watchdog Thread
        self._stop_watchdog = threading.Event()
        self._watchdog_thread = threading.Thread(target=self._socket_watchdog, args=(socket_path, self.active_pipeline, self._stop_watchdog), daemon=True)
        self._watchdog_thread.start()

        log.debug(f"AI Engine '{model_name}' on '{lens}': Waiting for socket {socket_path}...")
        return {"status": "loading", "model": model_name, "lens": lens}

    def set_log_level(self, level: str) -> dict:
        level = level.upper()
        if level in ["DEBUG", "INFO", "WARNING", "ERROR"]:
            logging.getLogger().setLevel(getattr(logging, level))
            log.info("Log level changed to %s", level)
            return {"status": "ok", "level": level}
        return {"status": "error", "message": f"Invalid level: {level}"}

    def _handle_sigterm(self, signum, frame):
        # Move cleanup to main loop to avoid reentrant logging calls
        GLib.idle_add(self._safe_stop)

    def _safe_stop(self):
        log.debug("Safe shutdown triggered...")
        if hasattr(self, 'loop'):
            self.loop.quit()
        return False

    def _request_shm_attach(self, lens, socket_path):
        """Request C engine to create the SHM branch. Non-fatal on failure."""
        try:
            bus = dbus.SessionBus()
            engine_proxy = bus.get_object('com.camera.UnifiedEngine', '/com/camera/UnifiedEngine')
            attach_method = engine_proxy.get_dbus_method('AttachSHM', 'com.camera.UnifiedEngine')
            attach_method(json.dumps({"lens": lens, "socket_path": socket_path}))
            log.info("Requested UnifiedEngine to AttachSHM for %s", lens)
            return True
        except dbus.exceptions.DBusException as e:
            log.warning("C Engine not reachable for AttachSHM: %s", e)
            return False
        except Exception as e:
            log.error("Failed to ask UnifiedEngine for SHM: %s", e)
            return False

    def _socket_watchdog(self, socket_path, pipeline, stop_event):
        """Thread that waits for the SHM socket and ensures pipeline is playing.
        Also retries AttachSHM every 30s if the socket doesn't appear."""
        log.debug("Watchdog started for socket: %s", socket_path)
        missing_socket_since = None
        last_shm_request = 0  # epoch of last AttachSHM retry
        while not stop_event.is_set():
            if os.path.exists(socket_path):
                missing_socket_since = None
                ret, state, pending = pipeline.get_state(0)
                if state == Gst.State.NULL:
                    log.debug("Source socket reappeared. Waiting 0.5s before initializing pipeline...")
                    time.sleep(0.5)
                    pipeline.set_state(Gst.State.PLAYING)
                elif state != Gst.State.PLAYING:
                    log.debug("Socket found! Waiting 0.5s, then starting AI Pipeline...")
                    time.sleep(0.5)
                    pipeline.set_state(Gst.State.PLAYING)
            else:
                ret, state, pending = pipeline.get_state(0)
                if state == Gst.State.PLAYING:
                    log.warning("Socket vanished. Reverting AI Pipeline to READY.")
                    pipeline.set_state(Gst.State.READY)

                # Track how long socket has been missing
                if not missing_socket_since:
                    missing_socket_since = time.time()

                elapsed = time.time() - missing_socket_since

                # Standby: socket missing > 10s → NULL to save CPU
                if elapsed > 10:
                    _, st, _ = pipeline.get_state(0)
                    if st != Gst.State.NULL:
                        log.info("Source socket missing for 10s. Entering Standby (NULL state).")
                        pipeline.set_state(Gst.State.NULL)

                # Retry AttachSHM every 30s if socket still missing
                if time.time() - last_shm_request > 30 and self.current_lens:
                    log.info("Socket still missing. Retrying AttachSHM for %s...", self.current_lens)
                    self._request_shm_attach(self.current_lens, socket_path)
                    last_shm_request = time.time()

            # Check for bus errors
            bus = pipeline.get_bus()
            msg = bus.pop_filtered(Gst.MessageType.ERROR)
            if msg:
                err, debug = msg.parse_error()
                log.error("AI Pipeline Error: %s", err)
                pipeline.set_state(Gst.State.NULL)
                time.sleep(1)
                pipeline.set_state(Gst.State.READY)

            time.sleep(1.0)
        log.debug("Watchdog stopped.")

    def stop_model(self, params: dict) -> dict:
        if hasattr(self, '_stop_watchdog'):
            self._stop_watchdog.set()
        if hasattr(self, '_watchdog_thread'):
            self._watchdog_thread.join(timeout=3.0)
        if self.active_pipeline:
            self.active_pipeline.set_state(Gst.State.NULL)
            self.active_pipeline = None
            log.debug("Stopped AI pipeline.")
            
        # Ask C engine to detach SHM natively
        if self.current_lens:
            try:
                bus = dbus.SessionBus()
                engine_proxy = bus.get_object('com.camera.UnifiedEngine', '/com/camera/UnifiedEngine')
                detach_method = engine_proxy.get_dbus_method('DetachSHM', 'com.camera.UnifiedEngine')
                detach_method(json.dumps({"lens": self.current_lens}))
                log.info(f"Requested UnifiedEngine to DetachSHM for {self.current_lens}")
            except dbus.exceptions.DBusException as e:
                log.warning(f"C Engine not reachable for DetachSHM (may already be stopped): {e}")
            except Exception as e:
                log.error(f"Failed to DetachSHM via D-Bus: {e}")

            # Publish empty to clear UI
            self.publisher.publish(TOPIC_AI_COORDS, {"lens": self.current_lens, "model": "", "coords": [], "ml_w": self.ml_input_w, "ml_h": self.ml_input_h})

        self.current_model = None
        self.interpreter = None
        self.tracker = None
        self.current_lens = None
        return {"status": "stopped"}

    def set_ai_config(self, config: dict) -> dict:
        """Dynamically update AI parameters without stopping the model.
        Supports: threshold, snapshots, overlay (proxied to C Engine).
        """
        result_config = {}
        lens = config.get("lens", self.current_lens or "lens1")
        result_config["lens"] = lens

        if "snapshots" in config:
            self.ai_snapshots_enabled = bool(config["snapshots"])
            result_config["snapshots"] = self.ai_snapshots_enabled

        if "threshold" in config:
            try:
                val = float(config["threshold"])
                if val < 0.0 or val > 1.0:
                    return {"status": "error", "message": f"Threshold must be 0.0-1.0, got {val}"}
                self.ai_threshold = val
                result_config["threshold"] = self.ai_threshold
            except (ValueError, TypeError) as e:
                return {"status": "error", "message": f"Invalid threshold value: {e}"}

        if "snapshot_threshold" in config:
            try:
                val = float(config["snapshot_threshold"])
                if val < 0.0 or val > 1.0:
                    return {"status": "error", "message": f"Snapshot threshold must be 0.0-1.0, got {val}"}
                self.snapshot_threshold = val
                result_config["snapshot_threshold"] = self.snapshot_threshold
            except (ValueError, TypeError) as e:
                return {"status": "error", "message": f"Invalid snapshot threshold: {e}"}

        # Overlay toggle — proxy to C Engine's ToggleOverlay
        if "overlay" in config:
            enabled = bool(config["overlay"])
            result_config["overlay"] = enabled
            try:
                bus = dbus.SessionBus()
                engine_proxy = bus.get_object('com.camera.UnifiedEngine', '/com/camera/UnifiedEngine')
                toggle_method = engine_proxy.get_dbus_method('ToggleOverlay', 'com.camera.UnifiedEngine')
                toggle_method(json.dumps({"lens": lens, "enabled": enabled}))
                log.info("Overlay %s for %s (proxied to C Engine)", "enabled" if enabled else "disabled", lens)
            except dbus.exceptions.DBusException as e:
                log.warning("C Engine not reachable for ToggleOverlay (is smartip_engine running?): %s", e)
                result_config["overlay_note"] = "C Engine not reachable, overlay state not applied"
            except Exception as e:
                log.error("Failed to proxy ToggleOverlay to C Engine: %s", e)

        log.info("AI: Config updated: %s", result_config)
        return {"status": "ok", "config": result_config}

    def get_status(self) -> dict:
        return {
            "current_model": self.current_model,
            "current_lens": self.current_lens,
            "ai_threshold": self.ai_threshold,
            "snapshots_enabled": self.ai_snapshots_enabled,
            "watchdog_active": hasattr(self, '_watchdog_thread') and self._watchdog_thread.is_alive()
        }

    def dump_dot(self, timestamp: str) -> dict:
        if not self.active_pipeline:
            return {"status": "error", "message": "No active pipeline"}
        
        dot_dir = os.environ.get("GST_DEBUG_DUMP_DOT_DIR", "/tmp")
        name = f"AI_ENGINE_{self.current_lens}_{self.current_model}_{timestamp}"
        
        try:
            Gst.debug_bin_to_dot_file(self.active_pipeline, Gst.DebugGraphDetails.ALL, name)
            log.info(f"AI DOT file dumped: {dot_dir}/{name}.dot")
            return {"status": "ok", "file": f"{name}.dot"}
        except Exception as e:
            log.error(f"Failed to dump AI DOT: {e}")
            return {"status": "error", "message": str(e)}

    def _get_delegates(self):
        os.makedirs("/tmp/npu_cache", exist_ok=True)
        os.environ["VIV_VX_CACHE_BINARY_GRAPH_DIR"] = "/tmp/npu_cache"
        os.environ["VIV_VX_ENABLE_CACHE_GRAPH_BINARY"] = "1"
        
        if os.path.exists("/usr/lib/libvx_delegate.so"):
            log.debug("NPU enabled via libvx_delegate.so")
            return [tflite.load_delegate("/usr/lib/libvx_delegate.so", options={"cache_dir": "/tmp/npu_cache"})]
        else:
            log.debug("NPU delegate not found, using CPU")
            return []

    def _init_tflite(self, model_path):
        try:
            delegates = self._get_delegates()
            self.interpreter = tflite.Interpreter(model_path, experimental_delegates=delegates)
                
            self.interpreter.allocate_tensors()
            self.input_details = self.interpreter.get_input_details()
            self.output_details = self.interpreter.get_output_details()
            self._dtype = self.input_details[0]['dtype']
            
            # Dynamically set ml_size from model input tensor shape (B11)
            self.ml_size = self.input_details[0]['shape'][1]
            log.info("Model loaded. Input tensor size: %dx%d", self.ml_size, self.ml_size)
            
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
            return True
        except Exception as e:
            log.error(f"TFLite Init Error: {e}")
            return False

    # --- Inference Loop ---

    def _letterbox(self, frame):
        h, w = frame.shape[:2]
        # self.ml_size = 320 # REMOVED (B11): Using value from _init_tflite
        scale = min(self.ml_size / w, self.ml_size / h)
        new_w, new_h = int(w * scale), int(h * scale)
        pad_x = (self.ml_size - new_w) // 2
        pad_y = (self.ml_size - new_h) // 2
        
        resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
        if not hasattr(self, '_letterbox_buf'):
            self._letterbox_buf = np.full((self.ml_size, self.ml_size, 3), 114, dtype=np.uint8)
        else:
            self._letterbox_buf.fill(114) # Clear previous
            
        self._letterbox_buf[pad_y:pad_y+new_h, pad_x:pad_x+new_w] = resized
        return self._letterbox_buf, scale, pad_x, pad_y

    def _on_new_sample(self, appsink):
        sample = appsink.emit("pull-sample")
        if not sample or not self.current_model or not self.interpreter:
            return Gst.FlowReturn.OK

        # Non-blocking lock: skip this frame if a model swap is in progress
        if not self._model_lock.acquire(blocking=False):
            return Gst.FlowReturn.OK

        if not hasattr(self, '_frame_count'): self._frame_count = 0
        self._frame_count += 1
        if self._frame_count % 150 == 0:
            log.info("AI: Active [%s] on %s", self.current_model, self.current_lens)

        buf = sample.get_buffer()
        caps = sample.get_caps()
        ret, map_info = buf.map(Gst.MapFlags.READ)
        if not ret: return Gst.FlowReturn.OK
        
        try:
            # 1. Process Frame
            snap_path = None # Fixed Scope (B12)
            frame = np.ndarray((self.ml_input_h, self.ml_input_w, 3), dtype=np.uint8, buffer=map_info.data)
            letterboxed, scale, pad_x, pad_y = self._letterbox(frame)
            
            if self._dtype == np.int8:
                input_tensor = (letterboxed.astype(np.int16) - 128).astype(np.int8).reshape(1, self.ml_size, self.ml_size, 3)
            else:
                input_tensor = letterboxed.reshape(1, self.ml_size, self.ml_size, 3)
                
            # 2. Inference
            self.interpreter.set_tensor(self.input_details[0]['index'], input_tensor)
            self.interpreter.invoke()
            # Fix TFLite Reference Error: copy the tensor so Python GC can release the TFLite internal buffer
            output = self.interpreter.get_tensor(self.output_details[0]['index']).copy()
            
            # 3. Model-specific Post-processing & Tracking
            # Pass ai_threshold to the tracker to allow lower-than-default detection
            coords, alerts = self.tracker.process(output, self.scale, self.zp, scale, pad_x, pad_y, self.ml_input_w, self.ml_input_h, threshold=self.ai_threshold)
            
            if coords and len(coords) > 0:
                log.debug("AI: Tracker returned %d raw coordinates.", len(coords))
            
            # 4. Thresholding
            filtered_coords = []
            
            if coords:
                for d in coords:
                    conf = d.get('conf', d.get('score', 0.0))
                    if conf >= self.ai_threshold:
                        filtered_coords.append(d)

            # 5. Dispatch Results (Always send heartbeat even if empty)
            now = time.time()
            has_detections = len(filtered_coords) > 0
            should_publish = has_detections or (now - self._last_coord_publish >= self._HEARTBEAT_INTERVAL)

            if should_publish:
                self.publisher.publish(TOPIC_AI_COORDS, {
                    "lens": self.current_lens,
                    "model": self.current_model,
                    "coords": filtered_coords,
                    "ml_w": self.ml_input_w,
                    "ml_h": self.ml_input_h
                })
                self._last_coord_publish = now
                
            # 6. Event-Driven AI Snapshot & Alert Dispatch
            if alerts:
                # Filter alerts based on snapshot_threshold (user request for stricter recording)
                filtered_alerts = [a for a in alerts if a.get('conf', 0.0) >= self.snapshot_threshold]
                
                if filtered_alerts:
                    if self.ai_snapshots_enabled:
                        try:
                            snap_dir = "/data/ai_snapshots"
                            os.makedirs(snap_dir, exist_ok=True)
                            # Use timestamp with milliseconds for uniqueness
                            snap_path = os.path.join(snap_dir, f"alert_{self.current_lens}_{self.current_model}_{int(time.time()*1000)}.jpg")
                            success = cv2.imwrite(snap_path, frame)
                            if success:
                                log.info("AI: Violation snapshot saved to %s", snap_path)
                            else:
                                log.warning("AI: cv2.imwrite failed for %s (disk full or write error?)", snap_path)
                                snap_path = None
                        except Exception as snap_err:
                            log.error("AI: Snapshot save failed: %s", snap_err)
                            snap_path = None

                for alert in filtered_alerts:
                    alert["lens"] = self.current_lens
                    alert["model"] = self.current_model
                    alert["timestamp"] = time.time()
                    if snap_path:
                        alert["snapshot"] = snap_path
                    try:
                        self.publisher.publish(TOPIC_ALERTS, alert)
                    except Exception as pub_err:
                        log.error("AI: Failed to publish alert: %s", pub_err)
                    
        except Exception as e:
             log.error(f"Inference error: {e}")
        finally:
             buf.unmap(map_info)
             self._model_lock.release()
             
        return Gst.FlowReturn.OK

    def run(self):
        import signal
        signal.signal(signal.SIGINT, self._handle_sigterm)
        signal.signal(signal.SIGTERM, self._handle_sigterm)

        DBusGMainLoop(set_as_default=True)
        self.dbus_service = AIEngineDBus(self)
        
        self.loop = GLib.MainLoop()
        try:
            self.loop.run()
        except Exception as e:
            log.error("Main loop error: %s", e)
        finally:
            self.stop_model({})
            log.info("AI Engine shut down")

if __name__ == "__main__":
    app = RealAIEngine()
    app.run()
