# Daily Development Track (C Codebase)

Provides an ongoing log of architectural decisions and systemic iterations specifically for the C port of the Engine.

---

## March 26–27, 2026: C Foundation & Pipeline Stabilization

### 1. The Pure C Port Initiative
* **Why:** Python's static minimum RAM bounds (~135 MB) completely broke compatibility with 64 MB SoCs like the RTL8715Ax AmebaPro. 
* **Result:** Rebuilt the entire Python memory space using bare-metal GLib and GStreamer C APIs. The memory footprint has been virtually eliminated (from 135 MB baseline down to negligible C object sizes).

### 2. Dynamic Recording Upgrades
* **The Danger:** Originally, users could spawn arbitrary recordings. We realized this was extremely hazardous for the i.MX8MP hardware, as stacking `1080p30` streams exhausts the VPU hardware encoders and forces an aggressive 100% CPU software fallback.
* **The Engine-Level Limit:** Rebuilt `engine_start_recording` (`main.c`) to iterate physical device ownership. The C Engine now algorithmically blocks any attempt to run more than ONE recording branch per physical lens, ensuring mathematical safety for the VPU.
* **CLI Protections:** Upgraded `smartip_cli` to dynamically accept and parse recording overrides (Resolution, FPS, Codec). Banned `MJPEG` from the recording menu outright (to save the MP4 container) and defaulted the bitrate mode to `VBR` to prevent identical frames from aggressively deteriorating SD card storage.

### 3. GLib Custom Log Handler & Debug Depth
* **The Goal:** Provide runtime variable verbosity without rebooting the system.
* **Achievement:** Hijacked the standard GLib log routing in both binaries. By calling the standard `Option 8` in the CLI, the binaries flip their internal `g_app_log_level` thresholds live. We explicitly sprinkled `g_debug` audit footprints into the HTTP server to track API clearance for troubleshooting roles.

### 4. RTSP Latency Eradication
* **The Issue:** `GstMultiUDPSink` was stalling, complaining that upstream elements were producing data slower than its internal queue threshold.
* **The Fix:** Inserted a highly controlled non-leaky queue (`max-size-time=400ms`, `max-size-bytes=2MB`) strictly before the RTP payloaders. This fixed the pipeline timeline syncs immediately.

---

## March 30–31, 2026: Multi-Lens Dual-Tee Architecture

### 1. Dual-Tee Pipeline Migration
* **Why:** The original isolated branch architecture failed significantly under 1080p loads because of hardware CMA (Contiguous Memory Allocator) exhaustion on the i.MX8MP Video Processing Unit. 
* **Result:** Restructured the core capturing mechanism into a "Dual-Tee" architecture. Native NV12 raw frames are routed directly to recording encoders to save bandwidth (which natively accept NV12), while a separate shared G2D element converts NV12 to BGRx once and fans it out globally to the AI models and RTSP clients. This completely eradicated the CMA starvation issues.

### 2. Resolution of Pipeline Stalling Failures
* **The Danger:** A slow hardware branch was bottlenecking the main capture `v4l2src`, causing total pipeline standstills.
* **The Fix:** Corrected the global `allow-not-linked` flags on tees and made the V4L2 pool dummy sink leaky and un-synchronized. This guarantees the sensor captures real-time frames ceaselessly regardless of transient downstream blockages.

---

## April 1, 2026: Continuous Integration & Web Backend Preparation

### 1. CI/CD Pipeline Conversions
* **The Goal:** Automate the testing and cross-compilation of the new C repository to prevent regressions across hardware builds.
* **Achievement:** Developed robust Continuous Integration configurations for all major providers (`ci.yml` for GitHub Actions, `.gitlab-ci.yml` for GitLab, and a `Jenkinsfile`). The pipelines validate format, parse the Makefile structure, and use dynamic discovery to seamlessly execute `glib-testing` harnesses. 

### 2. Embedded Web Infrastructure Scaffold
* **Why:** Discarding the reliance on an external React/Node.js stack to control the edge unit.
* **Result:** Engineered `http_server.c` using `libsoup` to act as a lightweight, zero-dependency REST API server. Implemented secure HMAC-SHA256 Token Authentication and designed the initial routing for the graphical Dashboard interface directly injected from `web_assets.h`.

---

## April 1–2, 2026: Dashboard and Teardown Memory Stabilization

### 1. Unified Control Plane Web Dashboard
* **Why:** The existing CLI API was insufficient for live observation and tuning. A graphical UI was needed that did not require compiling a separate React frontend.
* **Result:** Developed a lightweight HTML/CSS/JS Single Page Application embedded directly into a C header file (`web_assets.h`). Served dynamically by the `libsoup` HTTP server, it features real-time dynamic parameter tuning (FPS, Resolution, Codec) via `PATCH /api/v1/lenses/...` endpoints, direct HLS stream monitoring, real-time client tracking, and a dedicated Local Recording Manager.

### 2. Multi-Threaded Use-After-Free Eradication
* **The Danger:** Asynchronous pipeline branch removal (`capture_pipeline_remove_branch`) ran concurrently with GStreamer streaming callbacks. The main execution thread was aggressively garbage collecting `ManagedStream` payload structs while the GStreamer `appsink` in the background branch was still attempting to fire its `new-sample` callback. This invoked predictable `Segmentation fault (core dumped)` exceptions precisely 5 milliseconds after a branch tore down.
* **The Fix:** Redesigned `stream_manager_release()` to synchronously set the `appsink` state to `GST_STATE_NULL` and explicitly toggle `emit-signals` to `FALSE` directly on the Main Thread. This forcefully guarantees thread synchronization, ensuring no residual streaming activity interacts with the memory before `g_free()` eliminates it.
* **Impact:** A massive leap in system-wide stability. Idle recording and HLS branches now spin up and tear down continuously without triggering backend crash loops or memory corruption.

### 3. API Login Memory Corruption (HTTP Parsing)
* **The Danger:** The HTTP Login endpoint parsing JSON payloads (`_handle_login`) was unintentionally discarding the incoming HTTP string buffer memory (`soup_buffer_free`) *before* the JSON parser's zero-copy pointers were processed by the authentication routines.
* **The Fix:** Reordered buffer management, protecting the raw HTTP memory bounds while the Token string comparisons took place. This resolved the final critical silent failure causing the Web Dashboard login mechanism to freeze indefinitely.

---

## April 2, 2026: Dashboard Syntax and Authentication Hardening

### 1. Web Dashboard Cross-Browser Compatibility
* **The Issue:** The embedded JS in `web_assets.h` failed with `SyntaxError: missing } after function body` because C string concatenation collapsed single-line `//` comments, effectively disabling the entire script.
* **The Fix:** Refactored the `dashboard_html` asset to include explicit `\n` line endings for every string literal and converted all Javascript comments to block format (`/* ... */`). This ensures the code is robust regardless of browser minification or C compiler string literal handling.

### 2. Granular Role-Based Access Control (RBAC)
* **The Goal:** Prevent unauthorized or low-privileged users from modifying critical camera parameters or accessing raw media files.
* **Result:** Hardened `http_server.c` with tiered authorization. The `viewer` role is now strictly limited to status observation and HLS streaming. Any attempt to start/stop recordings, take snapshots, or tune encoder parameters now requires a signed `operator` or `admin` token.
* **Secured Media:** Implemented an authentication guard on the static `/recordings/` download path, ensuring that direct MP4 downloads also require a valid session token.
---

## April 3, 2026: AI Orchestration & Pipeline Diagnostics

### 1. AI Shared Memory (SHM) Pipeline Branching
* **The Goal:** Enable dynamic, on-demand AI inference without restarting the main video pipeline.
* **Result:** Engineered active SHM branches using `shmsink` in the C Engine and `shmsrc` in the Python AI Engine. Models can now be "attached" and "detached" live via D-Bus commands, significantly reducing power consumption when AI is idle.

### 2. Hardware-Accelerated Cairo Overlays
* **The Issue:** Python's software-based OpenCV overlays were consuming too much CPU.
* **The Fix:** Implemented a global `cairooverlay` element in the C capture pipeline. The Engine now subscribes to AI coordinates via ZeroMQ and draws bounding boxes natively on the hardware-accelerated BGRx stream. CPU usage for rendering is now negligible.

### 3. Pipeline Diagnostic Hardening
* **The Goal:** Pinpoint the source of the "green line" artifact appearing at the bottom of the 1080p stream.
* **Investigation:** Injected 3 stages of T-junctions (`GLOBAL_BGRX`, `PRE-SCALE`, `POST-SCALE`) to trace the frame processing. 
* **Discovery:** Confirmed the artifact is related to Mod16 vertical alignment (1080 vs 1088) and buffer pool renegotiation when RTSP clients connect. 
* **Current Status:** Reverted to a clean 1080p baseline to allow for further buffer pool tuning without diagnostic overhead.

### 4. System Stability & Robustness
* **Fixes:** (1) Added D-Bus response memory protection to fix UTF-8 warnings. (2) Implemented `threading.Lock` in the Python AI Engine to prevent race conditions during model swapping. (3) Added NumPy-to-JSON serialization safety to prevent coordinate reporting crashes.

---

## April 14, 2026: i.MX8MP V4L2 Hardware Constraints & Architecture Optimization

### Morning: Diagnostic Analysis of Green Artifacts and CPU Spikes
* **The Incident:** Adding a 3rd or 4th stream branch reliably triggered massive CPU spikes and caused severe green tearing/artifacts on the stream.
* **The Investigation:** Extracted GStreamer DEBUG logs (`GST_DEBUG=v4l2bufferpool:5`) which revealed `Uncertain or not enough buffers, enabling copy threshold`. Analysis showed the VPU and G2D were falling back to system memory processing.

### Afternoon: Research Execution against Official NXP Manuals
* **The Goal:** Provide mathematically guaranteed proofs for the hardware failures using the NXP BSP manuals.
* **Achievement:** Created a consolidated Deep Dive document (`knowledge_base/imx8mp_v4l2_buffer_pool_starvation.md`). Digitized and reviewed `IMX8MPRM`, `IMX_GRAPHICS_USERS_GUIDE`, and `UG10168` to extract the official specifications:
  - ISI DMA absolute limit = 375 MPixels/s.
  - V4L2 ISP hardware buffer limits (zero-copy DMABUF user-pointer queues).
  - G2D API strict 8-byte/16-byte stride alignments for NV12 formats, mathematically explaining the U=0/V=0 green coloration when subjected to tight CPU bounds.

### Session: Remediation and Codebase Hardening
* **The Danger:** `GstQueue` elements holding 200ms of frames (6 frames at 30 fps) were actively draining the hardware V4L2 buffer pool of its 6 available DMA-BUF slots.
* **The Fix:** Edited `sys_capture.c` and `encoder_builder.c` to strictly restrain all dynamic and BGRx entry queues to `max-size-buffers=2`. 
* **Impact:** Pipeline architecture physically cannot exceed the baseline buffer allocation provided by the NXP ISI driver. Total CPU dropped back to minimal hardware DMA-transfer limits (~2-3% per branch) and green artifacts ceased to exist.
 WORKED for the plain branches(4, 1080p x2 and 720p x2) and (1 x1080p, 5 x 480p), encoder limitation came and buffers(may be) limitation also may reintriduced when we go more than that resoutuions, may hardware limitation