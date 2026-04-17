# Helmet Detection System - Complete Technical Flow Document

## Overview

This document explains the complete processing flow of the AI-based helmet detection system implemented in `helmet_final.py`. The system uses a parallel GStreamer pipeline architecture to achieve 30+ FPS real-time detection.

---

## System Architecture

```
                              ┌─────────────────────┐
                              │    📹 VIDEO INPUT   │
                              │  Camera/File/RTSP   │
                              └──────────┬──────────┘
                                         │
                                         ▼
                              ┌─────────────────────┐
                              │      DECODER        │
                              │      (vpudec)       │
                              └──────────┬──────────┘
                                         │
                                         ▼
                              ┌─────────────────────┐
                              │    G2D CONVERT      │
                              │    → BGRx (GPU)     │
                              └──────────┬──────────┘
                                         │
                                         ▼
                              ┌─────────────────────┐
                              │        TEE          │
                              │   (Stream Split)    │
                              └────┬───────────┬────┘
                                   │           │
         ┌─────────────────────────┘           └─────────────────────────┐
         │                                                               │
         ▼                                                               ▼
┌─────────────────────┐                                     ┌─────────────────────┐
│   DISPLAY BRANCH    │                                     │     ML BRANCH       │
├─────────────────────┤                                     ├─────────────────────┤
│ Queue (leaky)       │                                     │ Queue (max-buf=10)  │
│       ↓             │                                     │       ↓             │
│ CairoOverlay        │◄─────── Detection Metadata ─────────│ G2D Resize 640x360  │
│ (Draw Boxes)        │                                     │       ↓             │
│       ↓             │                                     │ videoconvert→RGB    │
│ Display/Encode      │                                     │       ↓             │
└─────────────────────┘                                     │ AppSink             │
                                                            └──────────┬──────────┘
      ▲ 30 FPS OUTPUT                                                  │
                                                                       ▼
                                                            ┌─────────────────────┐
                                                            │  PYTHON PROCESSING  │
                                                            ├─────────────────────┤
                                                            │ _on_ml_sample()     │
                                                            │       ↓             │
                                                            │ Letterbox 320x320   │
                                                            │       ↓             │
                                                            │ NPU Inference       │
                                                            │ (YOLOv8 TFLite)     │
                                                            │       ↓             │
                                                            │ Post-process (NMS)  │
                                                            │       ↓             │
                                                            │ IoU Tracker         │
                                                            │       ↓             │
                                                            │ self.detections     │
                                                            └─────────────────────┘
```

---

## Component Details

### 1. Video Input Stage

| Component | Function |
|-----------|----------|
| `v4l2src` | Captures from USB/CSI camera |
| `filesrc` | Reads video file |
| `decodebin` | Auto-detects and decodes video format |

**Pipeline String (Camera):**
```
v4l2src device=/dev/video3 ! video/x-raw,width=1920,height=1080,framerate=30/1
```

**Pipeline String (File):**
```
filesrc location=video.mp4 ! decodebin
```

---

### 2. G2D Color Conversion (GPU)

**Purpose:** Convert camera's native format to BGRx for processing.

```
┌───────────────┐         ┌───────────────┐
│   YUY2/NV12   │  ────►  │     BGRx      │
│   (Camera)    │  G2D    │ (4 bytes/px)  │
└───────────────┘  GPU    └───────────────┘
```

**Why BGRx?**
- Cairo overlay requires BGRx format for drawing
- 4-byte alignment is efficient for GPU
- Alpha channel (x) is ignored

**Pipeline Element:**
```
imxvideoconvert_g2d ! video/x-raw,format=BGRx
```

---

### 3. TEE (Stream Splitter)

**Purpose:** Split single video stream into two parallel branches.

```
                    ┌─────────────────────────┐
                    │     Display Branch      │
                    │       (30 FPS)          │
                    └─────────────────────────┘
                              ▲
                              │
              ┌───────────────┴───────────────┐
              │              TEE              │
              └───────────────┬───────────────┘
                              │
                              ▼
                    ┌─────────────────────────┐
                    │       ML Branch         │
                    │      (Parallel)         │
                    └─────────────────────────┘
```

**Key Insight:** Display doesn't wait for ML. Both run independently!

---

### 4. Display Branch (CairoOverlay)

#### 4.1 Queue Configuration
```
queue max-size-buffers=2 leaky=downstream
```
- `max-size-buffers=2`: Hold max 2 frames
- `leaky=downstream`: Drop old frames if blocked

#### 4.2 CairoOverlay Drawing

**Callback Function:** `_draw_overlay()`

```python
def _draw_overlay(self, overlay, ctx, ts, dur):
    # Read latest detections (from ML branch)
    with self.lock:
        tracked = self.detections.copy()
    
    # Draw each detection
    for tid, d in tracked.items():
        x1, y1, x2, y2 = d['bbox']
        
        # Set color based on class
        if d['cid'] == 0:  # With Helmet
            ctx.set_source_rgb(0, 1, 0)  # Green
        elif d['cid'] == 1:  # Without Helmet
            ctx.set_source_rgb(1, 0, 0)  # Red
        
        # Draw bounding box
        ctx.rectangle(x1, y1, x2-x1, y2-y1)
        ctx.stroke()
        
        # Draw label
        ctx.show_text(f"ID{tid}: {CLASS_NAMES[d['cid']]}")
```

**CairoOverlay Flow:**
```
┌─────────────────┐
│  Frame arrives  │
└────────┬────────┘
         ▼
┌─────────────────┐
│ _draw_overlay() │
│    called       │
└────────┬────────┘
         ▼
┌─────────────────┐
│ Read detections │
│ from memory     │
└────────┬────────┘
         ▼
┌─────────────────┐
│ For each detect │
│ - Draw rectangle│
│ - Draw label    │
└────────┬────────┘
         ▼
┌─────────────────┐
│ Frame continues │
│ to display      │
└─────────────────┘
```

---

### 5. ML Branch (AI Processing)

#### 5.1 G2D Resize (GPU)

**Purpose:** Reduce frame size for faster ML processing.

```
┌─────────────────┐         ┌─────────────────┐
│   1920 x 1080   │  ────►  │    640 x 360    │
│   (Full HD)     │  G2D    │   (Smaller)     │
└─────────────────┘  GPU    └─────────────────┘
```

**Why 640x360?**
- Maintains 16:9 aspect ratio (same as source)
- Uniform scaling = GPU efficient
- Small enough for fast CPU letterbox

**Pipeline Element:**
```
imxvideoconvert_g2d ! video/x-raw,width=640,height=360,format=RGB16
```

#### 5.2 RGB16 to RGB Conversion

```
videoconvert ! video/x-raw,format=RGB
```

**Why CPU here?**
- RGB16 is G2D's efficient format
- Model needs standard RGB (24-bit)
- Small frame (640x360) = fast conversion

#### 5.3 AppSink (Python Bridge)

```python
appsink name=ml_sink emit-signals=true drop=false max-buffers=10 sync=false
```

| Property | Value | Purpose |
|----------|-------|---------|
| `emit-signals` | true | Call Python callback |
| `drop` | false | Process ALL frames |
| `max-buffers` | 10 | Queue limit |
| `sync` | false | Don't wait for display |

---

### 6. Python ML Processing

#### 6.1 Sample Callback

```python
def _on_ml_sample(self, appsink):
    # Get frame from GStreamer
    sample = appsink.emit("pull-sample")
    buffer = sample.get_buffer()
    
    # Map buffer to numpy array
    ret, mem = buffer.map(Gst.MapFlags.READ)
    frame = np.ndarray((h, w, 3), dtype=np.uint8, buffer=mem.data).copy()
    buffer.unmap(mem)
    
    # Process frame
    raw_detections = self._inference(frame)
    tracked = self.tracker.update(raw_detections)
    
    # Update shared detections
    with self.lock:
        self.detections = tracked
```

---

### 7. Preprocessing (Letterbox)

**Purpose:** Convert 640x360 frame to 320x320 model input with padding.

```
┌─────────────────┐       ┌─────────────────┐       ┌─────────────────┐
│   640 x 360     │ ────► │   320 x 180     │ ────► │   320 x 320     │
│   (Input)       │ Resize│  (Half size)    │  Pad  │ (Model input)   │
└─────────────────┘       └─────────────────┘       └─────────────────┘
```

**Letterbox Code:**
```python
def _letterbox(self, frame):
    # Precomputed values for 640x360 → 320x320
    new_w, new_h = 320, 180  # Resized dimensions
    pad_y, pad_x = 70, 0     # Padding (top/bottom)
    scale = 0.5              # Scale factor
    
    # Resize to 320x180
    resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_NEAREST)
    
    # Create 320x320 buffer with gray (114) padding
    self._letterbox_buf = np.full((320, 320, 3), 114, dtype=np.uint8)
    
    # Place resized image in center
    self._letterbox_buf[pad_y:pad_y+new_h, pad_x:pad_x+new_w] = resized
    
    return self._letterbox_buf, scale, pad_x, pad_y
```

**Visual Representation:**
```
┌────────────────────────────────┐
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│ ← pad_y = 70 pixels (gray)
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│
├────────────────────────────────┤
│                                │
│   Original Image (320 x 180)   │
│                                │
├────────────────────────────────┤
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│ ← pad_y = 70 pixels (gray)
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│
└────────────────────────────────┘
         Total: 320 x 320
```

---

### 8. NPU Inference (YOLOv8)

**Purpose:** Run AI model on NPU hardware accelerator.

#### 8.1 Model Loading
```python
def _load_model(self, model_path):
    # Enable NPU cache for faster loading
    os.environ["VIV_VX_CACHE_BINARY_GRAPH_DIR"] = "/tmp/npu_cache"
    os.environ["VIV_VX_ENABLE_CACHE_GRAPH_BINARY"] = "1"
    
    # Load VX delegate for NPU acceleration
    delegate = tflite.load_delegate("/usr/lib/libvx_delegate.so")
    self.interpreter = tflite.Interpreter(model_path, experimental_delegates=[delegate])
    self.interpreter.allocate_tensors()
```

#### 8.2 Inference Execution
```python
def _inference(self, frame):
    # Letterbox preprocessing
    letterboxed, scale, pad_x, pad_y = self._letterbox(frame)
    
    # Quantization for INT8 model
    if self._dtype == np.int8:
        input_tensor = (letterboxed.astype(np.int16) - 128).astype(np.int8)
    
    # Run inference
    self.interpreter.set_tensor(self.input_details[0]['index'], input_tensor)
    self.interpreter.invoke()  # ← NPU processes here (~25ms)
    output = self.interpreter.get_tensor(self.output_details[0]['index'])
    
    return self._postprocess(output, scale, pad_x, pad_y)
```

**NPU Timing:**
| Operation | Time |
|-----------|------|
| Set tensor | ~1ms |
| Invoke (inference) | ~25ms |
| Get tensor | ~1ms |
| **Total** | **~27ms** |

---

### 9. Post-Processing

**Purpose:** Convert raw model output to detection boxes.

#### 9.1 Output Format (YOLOv8)
```
Model output shape: [1, 7, 2100]
                       │  │
                       │  └── 2100 candidate boxes
                       └── 7 = [cx, cy, w, h, class0, class1, class2]
```

#### 9.2 Post-Processing Steps

```
┌─────────────────────┐
│ Raw Output          │
│ [1, 7, 2100]        │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Dequantize          │
│ (if INT8 model)     │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Transpose           │
│ [2100, 7]           │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Filter by Confidence│
│ (> 0.20 threshold)  │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Calculate Box Coords│
│ (cx,cy,w,h → x1,y1) │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ NMS                 │
│ (Remove Overlaps)   │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Final Detections    │
└─────────────────────┘
```

#### 9.3 Coordinate Transformation

```python
# From normalized [0,1] to frame coordinates
cx = cx_norm * 320 - pad_x  # Remove padding offset
cy = cy_norm * 320 - pad_y
cx, cy = cx / scale, cy / scale  # Scale back to 640x360

# Scale to display resolution (1920x1080)
x1 = (cx - w/2) * (1920 / 640)
y1 = (cy - h/2) * (1080 / 360)
```

**Coordinate Flow:**
```
Model output (normalized 0-1)
           │
           ▼
Letterbox coords (320x320)
           │ Remove padding
           ▼
Resized coords (320x180)
           │ Scale by 2 (0.5⁻¹)
           ▼
ML input coords (640x360)
           │ Scale to display
           ▼
Display coords (1920x1080)
```

#### 9.4 Non-Maximum Suppression (NMS)

**Purpose:** Remove duplicate overlapping boxes.

```python
def _nms(self, boxes, scores, thresh=0.45):
    # Sort by confidence (highest first)
    order = scores.argsort()[::-1]
    
    keep = []
    while order.size > 0:
        i = order[0]
        keep.append(i)
        
        # Calculate IoU with remaining boxes
        iou = calculate_iou(boxes[i], boxes[order[1:]])
        
        # Keep boxes with IoU < threshold
        order = order[np.where(iou <= thresh)[0] + 1]
    
    return keep
```

**NMS Visualization:**
```
Before NMS:              After NMS:
┌─────────┐              ┌─────────┐
│ Box A   │              │ Box A   │  ← Highest confidence kept
│  ┌──────┼──┐           │         │
│  │ Box B│  │           │         │
└──┼──────┘  │           └─────────┘
   │         │           
   └─────────┘           Box B removed (IoU > 0.45)
```

---

### 10. Object Tracking (IoU-based)

**Purpose:** Assign persistent IDs to detections across frames.

#### 10.1 Tracker Algorithm

```
┌─────────────────────┐
│ New Frame Detections│
└──────────┬──────────┘
           ▼
    ┌──────────────┐
    │   Existing   │ ───► NO ───► Register all as new
    │   Objects?   │
    └──────┬───────┘
           │ YES
           ▼
┌─────────────────────┐
│ Calculate IoU Matrix│
│ (existing vs new)   │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Match by highest IoU│
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Update matched objs │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Register unmatched  │
│ new detections      │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Increment disappeared│
│ for unmatched old   │
└──────────┬──────────┘
           ▼
    ┌──────────────┐
    │ Disappeared  │ ───► YES ───► Deregister object
    │ > threshold? │
    └──────┬───────┘
           │ NO
           ▼
┌─────────────────────┐
│   Keep tracking     │
└─────────────────────┘
```

#### 10.2 Class Smoothing

**Purpose:** Prevent class flickering between frames.

```python
def get_stable_class(self, objectID):
    # Get history of last N frames
    history = self.class_history[objectID]  # deque of classes
    confidences = self.confidence_history[objectID]
    
    # Weight votes by confidence
    class_weights = {}
    for cls, conf in zip(history, confidences):
        class_weights[cls] = class_weights.get(cls, 0) + conf
    
    # Return class with highest weighted votes
    return max(class_weights, key=class_weights.get)
```

**Example Smoothing:**
```
Frame 1: Class=1 (Without Helmet), conf=0.85
Frame 2: Class=0 (With Helmet), conf=0.30  ← noise
Frame 3: Class=1 (Without Helmet), conf=0.90
Frame 4: Class=1 (Without Helmet), conf=0.88
Frame 5: Class=1 (Without Helmet), conf=0.92

Weighted sum:
  Class 0: 0.30
  Class 1: 0.85 + 0.90 + 0.88 + 0.92 = 3.55

Result: Class 1 (Without Helmet) ← Correct!
```

---

### 11. Data Flow Summary

```
┌─────────┐    ┌─────────┐    ┌─────────┐
│ CAMERA  │───►│  G2D    │───►│   TEE   │
│         │    │  GPU    │    │ (Split) │
└─────────┘    └─────────┘    └────┬────┘
                                   │
            ┌──────────────────────┼──────────────────────┐
            │                      │                      │
            ▼                      │                      ▼
┌─────────────────────┐            │         ┌─────────────────────┐
│   DISPLAY BRANCH    │            │         │     ML BRANCH       │
│                     │            │         │                     │
│ Frame (BGRx)        │            │         │ Frame (BGRx)        │
│      ↓              │            │         │      ↓              │
│ CairoOverlay        │            │         │ G2D Resize          │
│ (read detections)   │◄───────────┼─────────│      ↓              │
│      ↓              │   Shared   │         │ NPU Inference       │
│ Display with boxes  │ Detections │         │      ↓              │
│                     │            │         │ Tracker             │
│   [30 FPS OUT]      │            │         │      ↓              │
└─────────────────────┘            │         │ Update detections   │
                                   │         └─────────────────────┘
                                   │
                              (PARALLEL!)
```

---

### 12. Timing Analysis

| Stage | Hardware | Time | % of Frame |
|-------|----------|------|------------|
| G2D color convert | GPU | ~2ms | 6% |
| G2D resize | GPU | ~2ms | 6% |
| RGB conversion | CPU | ~1ms | 3% |
| Letterbox | CPU | ~2ms | 6% |
| NPU inference | NPU | ~25ms | 76% |
| Post-process | CPU | ~2ms | 6% |
| Tracking | CPU | ~1ms | 3% |
| **Total ML** | | **~35ms** | |
| **Display FPS** | | **30+ FPS** | Independent! |

---

### 13. Key Design Decisions

| Decision | Reason |
|----------|--------|
| **Parallel branches** | Display runs at 30 FPS without waiting for ML |
| **640x360 resize** | Maintains aspect ratio, GPU efficient |
| **Letterbox padding** | Avoids distortion, model sees correct shapes |
| **INT8 quantization** | NPU optimized, 4x faster than FP32 |
| **IoU tracking** | Stable IDs, no duplicate counting |
| **Class smoothing** | Prevents flickering labels |
| **Leaky queue** | Drops old frames, keeps display smooth |

---

## Summary

The helmet detection system achieves 30+ FPS by:

1. **Parallel architecture**: Display and ML run independently
2. **GPU acceleration**: G2D handles color conversion and resize
3. **NPU inference**: AI model runs on dedicated hardware
4. **Efficient preprocessing**: Letterbox maintains aspect ratio
5. **Smart tracking**: IoU-based with temporal smoothing

This architecture ensures smooth real-time video while performing accurate AI detection!
