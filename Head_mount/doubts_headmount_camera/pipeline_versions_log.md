# SEI Pipeline Development & Debug Log

This document tracks the incremental versions, configurations, and debugging steps taken to achieve a stable, low-latency (< 1ms sync tolerance) H.264 video pipeline with SEI metadata injection on the i.MX8MP UVC Gadget.

## The Core Challenge
The primary issue encountered during development was **hardware buffer starvation**. The V4L2 hardware encoder (`v4l2h264enc`) utilizes a very small, fixed pool of hardware-backed memory buffers (DMABUFs). If downstream elements (like RTSP retransmission queues or MP4 muxers) hold references to these actual hardware buffers, the encoder quickly runs out of memory, stalling the entire pipeline after roughly 50 frames.

---

## Version History & Step-by-Step Evolution

### Baseline: Version 1 (RTSP Only)
* **Configuration:** Simple RTSP pipeline without recording or advanced encoder properties.
* **Status:** `WORKING` (but with sync drift).
* **Issue:** Synchronization gap between IMU and Video frame was reported as almost 2ms or drifting. This occurred because the SEI injection relied on relative PTS timestamps instead of absolute kernel capture time.

### Step 1: Absolute Timestamp Calibration
* **Change:** Updated `uvc_sei_injection.h` (`camera_src_probe`). Calculated absolute kernel capture time using: `abs_kernel_time = pts + base_time`.
* **Status:** `WORKING`.
* **Result:** Sync latency verified as a highly stable ~1.75ms (representing the physical kernel-to-user-space IOCTL handoff).

### Step 2a: Colorspace & Format Requirements
* **Goal:** Add mandatory client caps (`bt709`, `4:2:0`).
* **Attempt 1 (Failed):** Added `colorimetry=bt709,format=I420`. The stream stalled after ~2 frames.
* **Debugging:** Split the properties to isolate the culprit. Discovered that forcing `format=I420` forces a CPU memory copy that breaks the V4L2 zero-copy DMA path.
* **Attempt 2 (Fixed):** Replaced `I420` with `NV12` (the camera's native 4:2:0 format that maintains zero-copy).
* **Status:** `WORKING` (1750 µs latency).

### Step 2b: Encoder Properties (GOP & B-Frames)
* **Change:** Added `video_gop_size=30,video_b_frames=0` to the `extra-controls` of the `v4l2h264enc`.
* **Status:** `WORKING` (Stable over 146+ frames, ~1795 µs latency).

### Step 2c: Encoder Output Profile/Level
* **Change:** Appended `level=(string)4.2` alongside `profile=high` in the encoder's output caps filter.
* **Status:** `WORKING` (Stable over 90+ frames, ~1795 µs latency).

### Step 3a: Buffer Handling Refactor (Deep Copy)
* **Change:** In `uvc_sei_injection.h`, replaced `gst_buffer_ref()` with `gst_buffer_copy_deep()` and copied timestamps via `gst_buffer_copy_into()`.
* **Reasoning:** To prepare for the recording `tee`, we must prevent downstream elements from holding hardware DMABUFs. By copying the small H.264 payload into user-space RAM, we instantly release the hardware buffer back to the encoder's pool.
* **Status:** `WORKING` (Stable over 338+ frames, ~1785 µs latency). Proved that the software copy does not negatively impact latency or stability.

### Step 3b: Recording Tee Integration
* **Change:** Modified `sei_server.c` to add a `tee` element, splitting the H.264 stream into both `rtph264pay` (RTSP) and `mp4mux ! filesink location=/root/recording.mp4 async=false` (Disk Recording).
* **Status:** `FAILED`.
* **Symptom:** Pipeline failed to start or immediately stalled. The `sei_client` exits without receiving any frames.
* **Next Steps for Step 3b:**
  1. Investigate if `async=false` on `filesink` is blocking the RTSP branch.
  2. Verify if `mp4mux` requires upstream `h264parse` to be pushed into the queue (e.g. `t. ! queue ! h264parse ! mp4mux`).
  3. Run the pipeline on the board with `GST_DEBUG=3` to capture the exact state change or linking failure log.
