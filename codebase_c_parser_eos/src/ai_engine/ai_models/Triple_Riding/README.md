# Triple Riding Detection System - Complete Technical Flow Document

## Overview

This document explains the complete processing flow of the AI-based triple riding detection system implemented in `triding.py`. The system uses a parallel GStreamer pipeline architecture to achieve 30+ FPS real-time detection of motorcycle triple riding violations.

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
                                                            │ MRA-Hybrid Detection│
                                                            │       ↓             │
                                                            │ IoU Tracker         │
                                                            │       ↓             │
                                                            │ self.tracked_objects│
                                                            └─────────────────────┘
```

---

## Detection Classes

| Class ID | Class Name | Purpose |
|----------|------------|---------|
| 0 | Person | Individual person on/around motorcycle |
| 1 | Motorcycle | Two-wheeled motor vehicle |

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
filesrc location=video.mp4 ! decodebin ! imxvideoconvert_g2d ! video/x-raw,width=1920,height=1080
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
    # Read latest tracked objects (from ML branch)
    with self.lock:
        tracked = self.tracked_objects.copy()
        raw = self.raw_detections.copy()
    
    # Draw header with FPS, frame count, motorcycle count
    ctx.show_text(f"Display | FPS: {self.fps:.1f} | Frame: {self.frame_count} | MCs: {mc_count}")
    
    # Draw warning banner if violations detected
    if violation_count > 0:
        ctx.show_text(f"⚠ TRIPLE RIDING VIOLATION DETECTED: {violation_count} ⚠")
    
    # Draw each tracked violation
    for tid, detection_data in tracked.items():
        x1, y1, x2, y2 = detection_data['union_bbox']
        riders = detection_data['riders']
        score = detection_data['score']
        
        # Red box for violations, Green for best saved violation
        ctx.rectangle(x1, y1, x2-x1, y2-y1)
        ctx.stroke()
        
        # Draw label
        ctx.show_text(f"ID{tid}: {riders} Riders ({score:.2f})")
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
│ Read tracked    │
│ objects memory  │
└────────┬────────┘
         ▼
┌─────────────────┐
│ For each detect │
│ - Draw union box│
│ - Draw rider cnt│
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
    
    # Store current frame for violation image saving
    with self.frame_lock:
        self.current_frame = frame.copy()
    
    # Run inference and get raw detections
    raw_detections = self._inference(frame)
    
    # Separate motorcycles and persons
    motorcycles = [d for d in raw_detections if d['cid'] == MOTORCYCLE_CLASS_ID]
    persons = [d for d in raw_detections if d['cid'] == PERSON_CLASS_ID]
    
    # Detect triple riding violations using MRA-hybrid algorithm
    triple_riding_detections = detect_triple_riding_mra_hybrid(
        motorcycles, persons, WIDTH, HEIGHT, self.frame_count
    )
    
    # Update tracker
    tracked = self.tracker.update(triple_riding_detections)
    
    # Save violation image if new best found
    self._save_absolute_best_violation_image(frame)
    
    # Update shared tracked objects
    with self.lock:
        self.tracked_objects = tracked
        self.raw_detections = {'motorcycles': motorcycles, 'persons': persons}
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
Model output shape: [1, 6, 2100]
                       │  │
                       │  └── 2100 candidate boxes
                       └── 6 = [cx, cy, w, h, class0(Person), class1(Motorcycle)]
```

#### 9.2 Post-Processing Steps

```
┌─────────────────────┐
│ Raw Output          │
│ [1, 6, 2100]        │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Dequantize          │
│ (if INT8 model)     │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Transpose           │
│ [2100, 6]           │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Filter by Confidence│
│ (> 0.15 threshold)  │
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
│ (Person + MC boxes) │
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

### 10. Triple Riding Detection (MRA-Hybrid Algorithm)

**Purpose:** Detect triple riding violations by analyzing person-motorcycle associations.

#### 10.1 Algorithm Overview

```
┌─────────────────────┐
│ For each Motorcycle │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Expand MC Bounding  │
│ Box (Search Zone)   │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Find Persons in     │
│ Expanded Zone (IoA) │
└──────────┬──────────┘
           ▼
    ┌──────────────┐
    │  Persons     │
    │   >= 2 ?     │
    └──────┬───────┘
           │ YES              │ NO (1 person)
           ▼                  ▼
┌─────────────────────┐  ┌─────────────────────┐
│ Triple Riding!      │  │ Check Oversized Box │
│ riders = count + 1  │  │ (multiple persons   │
└─────────────────────┘  │  in single box?)    │
                         └──────────┬──────────┘
                                    │
                              ┌─────┴─────┐
                              │ Oversized?│
                              └─────┬─────┘
                                YES │ NO
                                    ▼
                         ┌─────────────────────┐
                         │ Triple Riding!      │
                         │ riders = 3          │
                         └─────────────────────┘
```

#### 10.2 Expansion Parameters

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `MC_EXPANSION_RATIO_X` | 0.7 | Expand MC box width by 70% |
| `MC_EXPANSION_RATIO_Y_TOP` | 2.5 | Expand MC box upward by 250% |
| `RIDER_IOA_THRESHOLD` | 0.30 | Min overlap to associate person |
| `MIN_PERSONS_PER_MC` | 2 | Min persons for violation |

**Visual: Expanded Search Zone**
```
┌──────────────────────────────────┐
│                                  │  ← MC_EXPANSION_RATIO_Y_TOP = 2.5
│         Expanded Zone            │     (Search for riders above MC)
│                                  │
├──────────────────────────────────┤
│ ┌─────────────────────────────┐  │
│ │                             │  │
│ │      Motorcycle Box         │  │
│ │                             │  │
│ └─────────────────────────────┘  │
└──────────────────────────────────┘
  ↑                              ↑
  MC_EXPANSION_RATIO_X = 0.7     MC_EXPANSION_RATIO_X = 0.7
```

#### 10.3 IoA (Intersection over Area) Calculation

```python
def calculate_ioa(box1, box2):
    """Calculate Intersection over Area (person box)"""
    # box1 = expanded motorcycle box
    # box2 = person box
    
    xi1 = max(box1[0], box2[0])
    yi1 = max(box1[1], box2[1])
    xi2 = min(box1[2], box2[2])
    yi2 = min(box1[3], box2[3])
    
    inter_area = max(0, xi2 - xi1) * max(0, yi2 - yi1)
    person_area = (box2[2] - box2[0]) * (box2[3] - box2[1])
    
    return inter_area / person_area if person_area > 0 else 0.0
```

**Why IoA instead of IoU?**
- IoA measures how much of person box overlaps with MC zone
- Person doesn't need to fully contain MC
- Better for detecting riders on top of motorcycles

#### 10.4 Oversized Person Box Detection

**Purpose:** Detect when multiple riders appear as a single "blob" in detection.

```python
def is_oversized_person_box(person_bbox, mc_bbox=None):
    p_width = px2 - px1
    p_height = py2 - py1
    
    # Check minimum height (filter noise)
    if p_height < PERSON_MIN_HEIGHT:  # 70px
        return False
    
    aspect_ratio = p_width / p_height
    
    # Tall narrow box = multiple stacked riders
    if aspect_ratio < PERSON_ASPECT_RATIO_MAX_TALL:  # 0.80
        return True, "Tall box"
    
    # Wide box = multiple side-by-side riders
    if aspect_ratio > PERSON_ASPECT_RATIO_MIN_WIDE:  # 1.5
        return True, "Wide box"
    
    # Person taller than expected relative to MC
    if mc_bbox is not None:
        mc_height = mc_bbox[3] - mc_bbox[1]
        height_ratio = p_height / mc_height
        if height_ratio > PERSON_TO_MC_HEIGHT_RATIO:  # 1.9
            return True, "Taller than MC"
    
    return False, "Normal size"
```

**Oversized Box Examples:**
```
Normal Person:       Tall (Stacked):      Wide (Side-by-side):
┌────────┐           ┌────┐               ┌────────────────┐
│        │           │    │               │                │
│        │           │    │               │                │
│        │           │    │               └────────────────┘
│        │           │    │               
└────────┘           │    │               
                     │    │               
                     └────┘               
```

---

### 11. Object Tracking (IoU-based)

**Purpose:** Assign persistent IDs to violations across frames.

#### 11.1 Tracker Algorithm

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
│ (threshold: 0.5)    │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│ Update matched objs │
│ Track best violation│
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
    │ > 30 frames? │
    └──────┬───────┘
           │ NO
           ▼
┌─────────────────────┐
│   Keep tracking     │
└─────────────────────┘
```

#### 11.2 Tracking Parameters

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `IOU_THRESHOLD` | 0.5 | Min IoU for matching |
| `MAX_DISAPPEARED` | 30 | Frames before deregister |
| `SMOOTHING_FRAMES` | 5 | History for temporal smoothing |

#### 11.3 Best Violation Tracking

```python
# Track the highest confidence violation per object
if detections[j]['score'] > current_best_score:
    self.best_violation_data[objectID].update(detections[j])
```

**Purpose:** Always save the clearest/most confident violation image.

---

### 12. Violation Image Saving

**Purpose:** Save cropped violation images for evidence/review.

#### 12.1 Absolute Best Strategy

```python
def _save_absolute_best_violation_image(self, frame):
    # Find highest scoring violation across all tracked objects
    for track_id, data in self.tracker.best_violation_data.items():
        if data['score'] > max_score:
            max_score = data['score']
            best_data_overall = data
    
    # Only save if better than previous best
    if max_score > self.absolute_best_data['score']:
        # Crop violation area from frame
        crop = frame[py1:py2, px1:px2]
        
        # Save using Cairo (BGR to PNG conversion)
        surf.write_to_png(filepath)
        
        # Delete previous best (keep only one file)
        if previous_save_path:
            os.remove(previous_save_path)
```

**Filename Format:**
```
violation_ID{track_id}_conf{score:.3f}_riders{count}_{timestamp}.png
```

Example: `violation_ID2_conf0.875_riders3_20260105_123456_789.png`

---

### 13. Data Flow Summary

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
│                     │            │         │ MRA-Hybrid Detect   │
│   [30 FPS OUT]      │            │         │      ↓              │
└─────────────────────┘            │         │ Tracker             │
                                   │         │      ↓              │
                                   │         │ Save Violation Image│
                                   │         │      ↓              │
                                   │         │ Update tracked_objs │
                                   │         └─────────────────────┘
                                   │
                              (PARALLEL!)
```

---

### 14. Timing Analysis

| Stage | Hardware | Time | % of Frame |
|-------|----------|------|------------|
| G2D color convert | GPU | ~2ms | 6% |
| G2D resize | GPU | ~2ms | 6% |
| RGB conversion | CPU | ~1ms | 3% |
| Letterbox | CPU | ~2ms | 6% |
| NPU inference | NPU | ~25ms | 71% |
| Post-process | CPU | ~2ms | 6% |
| MRA-Hybrid Detection | CPU | ~1ms | 3% |
| Tracking | CPU | ~1ms | 3% |
| **Total ML** | | **~36ms** | |
| **Display FPS** | | **30+ FPS** | Independent! |

---

### 15. Key Design Decisions

| Decision | Reason |
|----------|--------|
| **Parallel branches** | Display runs at 30 FPS without waiting for ML |
| **640x360 resize** | Maintains aspect ratio, GPU efficient |
| **Letterbox padding** | Avoids distortion, model sees correct shapes |
| **INT8 quantization** | NPU optimized, 4x faster than FP32 |
| **IoU tracking** | Stable IDs, no duplicate counting |
| **IoA for association** | Better for person-on-motorcycle detection |
| **MRA-Hybrid algorithm** | Catches both multi-person AND oversized-box cases |
| **Leaky queue** | Drops old frames, keeps display smooth |
| **Absolute best saving** | Only one high-quality violation image saved |

---

### 16. Configuration Parameters

```python
# Display Resolution
WIDTH, HEIGHT = 1920, 1080

# Detection Thresholds
NMS_THRESHOLD = 0.45        # Non-max suppression IoU threshold
CONF_THRESHOLD = 0.15       # Minimum detection confidence

# Triple Riding Parameters
MIN_PERSONS_PER_MC = 2      # Minimum persons for direct violation
RIDER_IOA_THRESHOLD = 0.30  # Overlap threshold for person-MC association
MC_EXPANSION_RATIO_X = 0.7  # MC box horizontal expansion
MC_EXPANSION_RATIO_Y_TOP = 2.5  # MC box upward expansion

# Size-based Parameters
PERSON_MIN_HEIGHT = 70      # Minimum valid person height (pixels)
PERSON_TO_MC_HEIGHT_RATIO = 1.9   # Max person/MC height ratio
PERSON_ASPECT_RATIO_MAX_TALL = 0.80  # Max W/H for "tall" detection
PERSON_ASPECT_RATIO_MIN_WIDE = 1.5   # Min W/H for "wide" detection

# Tracking Parameters
SMOOTHING_FRAMES = 5        # Temporal smoothing history
MAX_DISAPPEARED = 30        # Frames before object deregistration
IOU_THRESHOLD = 0.5         # Tracking match threshold
```

---

### 17. Usage Examples

#### Camera Mode (Direct Display)
```bash
python3 triding.py -m saved_model_triding_320.tflite -v /dev/video3
```

#### Video File Mode
```bash
python3 triding.py -m saved_model_triding_320.tflite -v tride_1080.mp4
```

#### With Output Video Saving
```bash
python3 triding.py -m saved_model_triding_320.tflite -v tride_1080.mp4 -o output.mp4
```

#### RTSP Streaming Mode
```bash
python3 triding.py -m saved_model_triding_320.tflite -v /dev/video3 -r
# Connect with: vlc rtsp://<board-ip>:8554/triple
```

---

### 18. Output Files

| Directory/File | Purpose |
|----------------|---------|
| `Triple_ride_violation/` | Violation images saved here |
| `violation_ID*_conf*_riders*_*.png` | Cropped violation evidence |
| `output.mp4` (if -o specified) | Full processed video with overlays |

---

## Summary

The triple riding detection system achieves 30+ FPS by:

1. **Parallel architecture**: Display and ML run independently
2. **GPU acceleration**: G2D handles color conversion and resize
3. **NPU inference**: AI model runs on dedicated hardware
4. **Efficient preprocessing**: Letterbox maintains aspect ratio
5. **Smart detection**: MRA-Hybrid catches multi-person AND oversized-box violations
6. **Stable tracking**: IoU-based with temporal smoothing
7. **Evidence saving**: Best quality violation image automatically saved

This architecture ensures smooth real-time video while performing accurate AI-based triple riding violation detection!

---

## Files in Repository

| File | Description |
|------|-------------|
| `triding.py` | Main Python implementation |
| `saved_model_triding_320.tflite` | YOLOv8 TFLite model (320x320 input) |
| `tride_1080.mp4` | Test video (1080p) |
| `tride5_1080.mp4` | Additional test video (1080p) |
| `README.md` | This documentation file |
