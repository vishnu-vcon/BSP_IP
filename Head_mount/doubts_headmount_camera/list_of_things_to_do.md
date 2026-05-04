# IMU 250 Hz Frequency Analysis & Architectures

This document explores the client requirement of a **>= 250 Hz IMU sample rate** alongside the strict constraint of a **Maximum sync tolerance <= 1 ms** to the video frame.

## The Core Conflict
The video pipeline operates at **30 FPS (33.3 ms per frame)**.
An IMU operating at 250 Hz generates a sample every **4.0 ms**.

Because SEI metadata must be attached to a video frame, you cannot transmit 250 SEI NAL units per second. You must "batch" the IMU data (approx. 8 to 9 samples per frame). However, standard batching introduces synchronization gaps between the asynchronous camera shutter and the rigid 4ms IMU polling loop.

> [!WARNING]
> **Hardware Magnetometer Limitation**
> The ICM-20948 chip contains an AK09916 magnetometer. According to the AK09916 datasheet, its absolute maximum continuous Output Data Rate (ODR) is **100 Hz** (Continuous Measurement Mode 4). The magnetometer physically cannot produce 250 unique samples per second. Any 250 Hz model will result in the magnetometer values being duplicated across 2 or 3 consecutive samples. (Accel and Gyro will successfully provide unique 250 Hz data).

---

## Architectural Models & Trade-offs

### 1. Current Architecture: 30 Hz Frame-Locked Inline
The IMU is sampled synchronously (via IOCTL) inside a zero-latency GStreamer pad probe the exact microsecond the 30 FPS frame hits user-space.

*   **Pros:** 
    *   Mathematically guarantees sub-1ms sync tolerance.
    *   Extremely low CPU overhead and zero buffering logic.
*   **Cons:** 
    *   Fails the 250 Hz frequency requirement (provides exactly 30 Hz).

### 2. Standard Background Batching: 250 Hz Polling
A dedicated `pthread` polls the IMU every 4 ms and saves the data to a software array. When the 30 FPS frame arrives, the accumulated array (~8 samples) is embedded into the SEI payload.

*   **Pros:** 
    *   Meets the 250 Hz requirement.
    *   Relatively simple user-space implementation.
*   **Cons:** 
    *   **Fails the <= 1 ms sync tolerance requirement.** Because the camera shutter fires asynchronously to the 4ms polling loop, the closest IMU sample to the frame could be up to **2.0 ms** away (half the polling interval).

### 3. Hardware FIFO Batching
The ICM-20948 internal hardware FIFO is configured to automatically sample at exactly 250 Hz. When the 30 FPS frame arrives, the software issues a bulk IOCTL to drain the hardware FIFO (~8 samples).

*   **Pros:** 
    *   Lowest CPU polling overhead (interrupt driven).
    *   Perfect 250 Hz timing without thread jitter.
*   **Cons:** 
    *   Requires highly complex kernel driver (`icm20948.c`) modifications to manage FIFO overflow and watermarks.
    *   **Fails the <= 1 ms sync tolerance requirement** for the exact same reason as Model 2. The FIFO clock and Camera clock are independent, yielding up to a 2.0 ms alignment gap.

### 4. The Hybrid Array Model (The Ultimate Solution)
To satisfy **both** the 250 Hz requirement and the <= 1 ms sync requirement, a Hybrid approach must be used that combines Model 1 and Model 2.

1.  **Background Polling:** A thread (or FIFO) polls the IMU every 4 ms to build a high-frequency history array.
2.  **Inline Frame Sync:** When the 30 FPS frame interrupt fires, the GStreamer pad probe instantly triggers a synchronous IMU IOCTL read.
3.  **SEI Array Payload:** The SEI payload structure is expanded into an array. `Sample[0]` is the inline frame-synced read. `Sample[1..8]` are the background polled samples spanning the time since the previous frame.

*   **Pros:** 
    *   Satisfies **all** client requirements simultaneously. `Sample[0]` guarantees < 1 ms sync tolerance to the frame, while the array provides the high-frequency 250 Hz curvature.
*   **Cons:** 
    *   Most complex software architecture to implement (requires mutex locks between the poller and the probe).
    *   Increases SEI payload size significantly (41 bytes * 9 samples = ~369 bytes per frame).
    *   The PC client extraction script (`sei_client.c`) must be rewritten to iterate through dynamic struct arrays.

---

## 5. Parallel Streaming (Out-of-Band)
Instead of embedding IMU data *inside* the video stream (SEI), the IMU data is transmitted in parallel alongside the video.

*   **How it works:** A background thread polls the IMU at exactly 250 Hz. Instead of waiting for a video frame, this thread immediately pushes the IMU data out over a dedicated TCP or UDP socket. The PC client runs the RTSP stream for video, and connects to the TCP socket to receive the 250 Hz IMU telemetry independently.
*   **Pros:** Perfectly solves the 250 Hz frequency problem. The IMU polling is completely decoupled from the 30 FPS video constraints. Zero impact on video encoding performance.
*   **Cons:** RTSP (video) and TCP (telemetry) are transmitted over two different network channels. They may experience different network delays (jitter). To synchronize them on the PC, the client must use the `CLOCK_MONOTONIC` timestamps to manually align the data during playback.

## 6. Option A: Sidecar Files (MP4 + CSV + JSON)
If the goal is purely **recording** (or analyzing post-capture) rather than live RTSP streaming, Option A completely bypasses the SEI complexities.

*   **How it works:**
    1.  **`video.mp4`**: The GStreamer pipeline records the 30 FPS video to an MP4 file. No SEI injection is performed.
    2.  **`imu.csv`**: A dedicated C program loops at 250 Hz, reading the IMU and writing it directly to a CSV file. Every row contains the exact `CLOCK_MONOTONIC` timestamp: `[Monotonic_ns, Accel_X, Accel_Y, ...]`
    3.  **`meta.json`**: A static file containing the Device ID, Environment, and crucial synchronization data.
*   **How Synchronization is Proven (The Math):**
    Because the MP4 video timestamps start at `0`, we must record the exact `CLOCK_MONOTONIC` base time when the recording started into `meta.json`. 
    When analyzing the data, for any frame at time $T_{relative}$ in the MP4, its true hardware time is:
    $$T_{absolute} = T_{base\_time} + T_{relative}$$
    You simply look up $T_{absolute}$ in the `imu.csv` to find the exact IMU samples captured at that microsecond.
*   **Pros:** Trivially supports 250 Hz (or even 1000 Hz) without any SEI limitations. Standardized format (CSV) makes data science and Python analysis extremely easy.
*   **Cons:** You now have 3 separate files to manage instead of 1 beautifully encapsulated file. If the files get separated or the `meta.json` is lost, the video and CSV can never be synchronized again.

## 7. TODO: Resolve `tee` + `mp4mux` Pipeline Failure (Step 3b)
We attempted to add a `tee` element to split the H.264 stream into both an RTSP stream (`rtph264pay`) and an MP4 recording (`mp4mux ! filesink`). The pipeline string was:
```
... h264parse ! tee name=t t. ! queue ! rtph264pay name=pay0 pt=96 t. ! queue ! mp4mux ! filesink location=/root/recording.mp4 async=false
```
**Symptom:** The pipeline failed to start or immediately stalled. `sei_client` exits immediately without receiving any frames. 
**Next steps to investigate:**
1. Check if `async=false` on the `filesink` is causing the recording branch to block the RTSP branch.
2. Verify if `mp4mux` is correctly receiving the stream or if it requires additional caps parsing (e.g. `queue ! h264parse ! mp4mux`).
3. Run the pipeline with `GST_DEBUG=3` on the board to capture the exact failure reason (e.g., linking error or state change failure).
