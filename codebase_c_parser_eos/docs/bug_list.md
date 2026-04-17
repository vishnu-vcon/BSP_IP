# Outstanding Bugs List (C Codebase)

This document tracks known issues and resolved bugs in the pure C port of the SMART IP Edge Application.

## Resolved (C Port)

> **Note on AI Implementation:** The core Python AI Engines (`ai_engine.py` and tracking models) are decoupled from the C Engine and are fully subjected to and dependent on the external AI team's development lifecycle. Bugs specific to model accuracy, PyTorch/ONNX logic, or tracker heuristics are handled upstream.

### 1. GStreamer Native Pipeline Bottleneck (i.MX8MP)
**Status:** ✅ RESOLVED IN C PORT
**Resolution:** The C codebase natively omits the `cairooverlay` software renderer. Hardware paths are now preserved end-to-end, bringing total application CPU down dramatically.

### 2. RTSP `multiudpsink` Latency Deadlocks
**Status:** ✅ RESOLVED
**Description:** The pipeline was throwing "Pipeline construction is invalid, please add queues" and causing 2-second delays on RTSP streams.
**Resolution:** Explicitly added bounded queues (`max-size-time=400ms`, `max-size-bytes=2MB`) upstream of the RTP payloaders in `rtsp_server.c`. This physically caps memory usage and perfectly satisfies the `multiudpsink` processing deadlines without OOM risks.

### 3. Pipeline DOT Graph Exports Missing
**Status:** ✅ RESOLVED
**Description:** DOT graphs weren't mapping correctly due to chaotic nanosecond timestamps applied to the filenames by `GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS`.
**Resolution:** Shifted to the standard `GST_DEBUG_BIN_TO_DOT_FILE` macro in `capture_pipeline.c`. Graphs are now predictably generated as `/tmp/pipeline_CAPTURE_START_video3.dot`.

### 4. JSON Decode Error on Empty Recording Start
**Status:** ✅ RESOLVED
**Description:** CLI crashed when starting recordings.
**Resolution:** Fully rebuilt the `do_recording` CLI menu and the Control Plane API to gracefully parse custom JSON block overrides. 

### 5. MP4 Container Corruption via MJPEG
**Status:** ✅ RESOLVED
**Resolution:** Restrained the recording menu in the CLI to explicitly forbid MJPEG selection, as MP4 containers cannot properly wrap MJPEG frames.

### 37. V4L2 Buffer Pool Starvation causing CPU Spikes and Green Screen Tearing
**Status:** ✅ RESOLVED (2026-04-14)
**Root Cause:** Adding multiple dynamic branches (RTSP/Recording/HLS) caused entry queues (`max-size-buffers=0`, `max-size-time=200ms`) to hoard `GstBuffer`s. With the i.MX8MP ISI driver strictly limited to 6 hardware DMA-BUF slots, the queues actively starved the hardware pool. GStreamer's fallback mechanism executed a deep copy (`memcpy`) to system RAM. Because this cpu-copy packed memory tightly without respecting the NXP G2D 8-byte/16-byte stride alignment requirements, subsequent hardware blocks interpreted the corrupted Y/UV planes as U=0, V=0 (Green). 
**Resolution:** Hard-capped all downstream entry queues (`bgrx_q_g2d`, `q_cairo`, and branch `q`) to `max-size-buffers=2` and `max-size-time=0`. This forces userspace to rapidly cycle V4L2 buffers back to the driver, keeping the hardware DMA pipeline saturated and entirely eliminating the forced CPU memcpy.

---

## Open Issues

### 10. Corrupt/Empty Recording at Low FPS (10fps)
**Status:** ✅ RESOLVED
**Description:** Recording at 10fps produced tiny (19KB) or unplayable files due to (1) encoder starvation from a too-short 100ms queue, (2) missing GOP size optimization for low FPS, and (3) missing caps negotiation.
**Resolution:** 
1. Added `fps_caps` capsfilter for explicit negotiation.
2. Increased branch queue `max-size-time` from 100ms to 1s in `encoder_builder.c`.
3. Set `video_gop_size` = `fps` for hardware encoders via `extra-controls`.
4. Enabled `async-finalize=TRUE` on `splitmuxsink` in `recording.c`.
Now 10fps recordings produce playable files with correct headers.

### 1. Software Encoder Fallback
**Status:** ✅ CLOSED (Disproven)
**Description:** DOT files from the board conclusively show `v4l2h264enc`, `v4l2h265enc`, and `imxvideoconvert_g2d` are ALL active. Zero software fallback. GST_PERFORMANCE logs confirm zero ongoing full-frame memcpy.

### 2. HTTP MJPEG Streaming Browser Display
**Status:** PENDING VERIFICATION
**Description:** Need to verify if the raw HTTP MJPEG stream (port 8080 or via Control proxy) actually renders smoothly in a Google Chrome/Firefox tab without boundary header tearing.

### 3. High CPU at 4Mbps (40% Observed)
**Status:** OPEN (Under Investigation)
**Severity:** MEDIUM
**Description:** Running lens1/main at 1080p/25fps/4Mbps showed ~40% CPU (400/1000 in `top`). This is unusually high for a hardware-accelerated VPU encode.
**Possible Causes:** (1) Software scaler fallback instead of G2D, (2) Excessive bitrate forcing VPU to work harder, (3) Capture pipeline processing overhead.
**Next Steps:** Check DOT files to confirm G2D is active. Compare CPU at 2Mbps vs 4Mbps.

---

### 12. JSON Assertion Failure on `lens1/third` Configuration
**Status:** ✅ RESOLVED
**Root Cause:** The Python CLI (`smartip_cli.py`) was sending `{"third": True}` to enable the third stream. However, the C engine expects an object `{"third": {}}` so it can parse inner default properties (like resolution and FPS). `json_object_get_object_member` failed because a boolean is not an object.
**Resolution:** Updated `smartip_cli.py` to send `params["third"] = {}`.

### 13. Pipeline Stalling (`v4l2src` Deadlock via `capture_tee`)
**Status:** ✅ RESOLVED
**Root Cause:** The `dummy_q` element (which keeps the V4L2 pool active) was mistakenly configured without `leaky=downstream`, and its terminating `fakesink` had `sync=TRUE`. If hardware timestamps from a sensor (`/dev/video4`) drifted slightly ahead of the system clock, `fakesink` would sleep to synchronize, filling the 2-buffer `dummy_q`. Once full, this blocked the entire `capture_tee`, freezing `v4l2src` and causing all active RTSP/recording branches to stall.
**Resolution:** Set `leaky=downstream` on `dummy_q` and disabled clock synchronization (`sync=FALSE`) on `dummy_sink` in `capture_pipeline.c`.

### 14. Lens 2 Stability (Hardware Resource Contention)
**Status:** ✅ RESOLVED (Architecture Stabilized)
**Root Cause:** In Dual ISP mode, bandwidth contention for high-bitrate BGRx buffers can cause jitter. While BGRx is mandatory for AI/Overlay compatibility, its high memory footprint (32bpp) makes the pipeline sensitive to any branch blockages.
**Resolution:** Acknowledged BGRx requirement. The "stall" was definitively fixed by the `dummy_q` leaky/non-sync update (Bug 13), which ensures the V4L2 pool never blocks even under high bandwidth/bus contention.

## Resolved (Live Hardware Test — 2026-03-30)

### 11. Invalid UTF-8 in D-Bus / API Logs
**Status:** ✅ RESOLVED
**Root Cause:** The JSON parser was unrefed *before* the string pointers (username, lens, branch) extracted from it were used to build responses or logs. This created dangling pointers and memory garbage.
**Resolution:** Reordered cleanup in `main.c` (Engine) and `http_server.c` (Control Plane) to ensure the parser lives until the output strings are safely copied/logged.

### 6. `vbr-mode` Property Not Found on Unpatched Kernels
**Status:** ✅ RESOLVED
**Resolution:** Added `g_object_class_find_property()` runtime probe before every `vbr-mode` set call.

### 7. Recording Start Crashes Python CLI (JSONDecodeError)
**Status:** ✅ RESOLVED
**Resolution:** Hardened `smartip_cli.py` `_request()` with `errors="replace"` and try/except fallback.



### 8. RTSP Segfault on Reconnect After Dynamic Config Update
**Status:** ✅ RESOLVED (2 fixes)
**Root Cause:** (1) Dangling RTSP factory pointer after codec-change teardown. (2) 5s async teardown timer applied to ALL branches — RTSP clients reconnecting within 5s hit stale element pointers.
**Resolution:** (1) Added `gst_rtsp_mount_points_remove_factory()` before teardown in codec-change path. (2) Split `_unlink_probe_cb` to use **immediate synchronous teardown** for RTSP branches (`appsink` sink) and keep 5s async delay only for recording branches (`splitmuxsink`).

### 9. No Application-Only Log Level
**Status:** ✅ RESOLVED
**Resolution:** Implemented `APP` log level that silences GStreamer noise. Usage: `log APP` from CLI.

### 15. Fatal `_on_bus_error` Crashes Process on Transient Errors
**Status:** ✅ RESOLVED
**Root Cause:** `_on_bus_error` in `capture_pipeline.c` used `g_error()` — a GLib macro that calls `abort()`. Any GStreamer ERROR message on the bus (including transient `not-linked` during branch teardown) killed the entire process with a core dump.
**Resolution:** Replaced `g_error()` with `g_warning()` so transient pipeline errors are logged but not fatal.

### 16. Recording Branch Stalls All RTSP Streams (`tee` Missing `allow-not-linked`)
**Status:** ✅ RESOLVED
**Root Cause:** The `capture_tee` in `capture_pipeline.c` defaulted to `allow-not-linked=FALSE`. When a recording branch's `splitmuxsink` returned `GST_FLOW_NOT_LINKED` during its internal muxer initialization, the tee propagated this error upstream to `v4l2src`, which stopped producing frames — starving ALL branches (RTSP + recording).
**Resolution:** Set `allow-not-linked=TRUE` on `capture_tee`. The tee now ignores `NOT_LINKED` from transiently-unready branches and continues delivering to healthy ones.

### 17. Destructive `_save_user_config` Erases Non-RTSP Fields
**Status:** ✅ RESOLVED
**Root Cause:** `_save_user_config` in `main.c` rebuilt the entire `user_overrides` JSON from scratch using only RTSP data. Any `recording` or `ai` nodes were silently wiped on every save.
**Resolution:** Rewrote to a non-destructive "JSON Merger" that loads existing config and updates only the changed fields. Recording configs are now preserved, and recordings never auto-start on reboot.

### 18. G2D Green Line / Red Flickering in Dynamic Branches
**Status:** ✅ RESOLVED
**Root Cause:** `imxvideoconvert_g2d` was being instantiated inside each dynamic branch (encoder_builder.c). When the G2D is first initialized mid-stream during a dynamic attachment, it causes stride/padding mismatches and caps renegotiation artifacts: **green edges** and **red color flickering**. This was already solved in the Python codebase (`unified_engine.py:174`: "Initializing G2D hardware at pipeline boot prevents the green line").
**Resolution:** Moved the G2D format conversion (NV12→BGRx) into `capture_pipeline.c` as a permanent element before the tee. Pipeline is now: `v4l2src → src_caps(NV12) → capture_g2d → capture_g2d_caps(BGRx) → tee`. Branch G2D instances now only do resolution scaling (BGRx→BGRx), which is safe in dynamic pipelines.

### 19. Branch Teardown Deadlocks Tee Streaming Thread (Stalls ALL Branches)
**Status:** ✅ RESOLVED
**Root Cause:** `_unlink_probe_cb` in `capture_pipeline.c` called `gst_element_set_state(NULL)` and `gst_bin_remove()` INSIDE the pad probe callback for RTSP branches. The Python codebase explicitly warned: "Calling set_state(NULL) inside the probe blocks the tee's streaming thread, which deadlocks ALL other tee branches (e.g. RTSP freezes)." Additionally, no EOS probe was used — teardown was blindly scheduled without confirming EOS had propagated.
**Resolution:** Rewrote teardown to match Python pattern: (1) ALL branches use async teardown via `g_timeout_add` — NEVER synchronous inside the probe. (2) Added `_eos_arrived_probe` (EVENT_DOWNSTREAM on last element's sink pad) to detect EOS completion before scheduling teardown. (3) Recording: 5s delay after EOS (moov atom), RTSP: 500ms delay, 10s fallback for both.

### 20. Dynamic Recording Branch — Multi-Resolution & Multi-Codec Stability
**Status:** 🔶 SEMI-RESOLVED (BGRx: 720p only | NV12: All resolutions)

**Description:** Adding a dynamic recording branch with `splitmuxsink` to a live 1080p pipeline caused multiple failure modes depending on architecture and pixel format.

### 21. HLS Web Dashboard Latency & Video Cropping
**Status:** ✅ RESOLVED
**Description:** Web player video was exhibiting extreme lag / stalling on initial connection and the stream appeared cropped despite native resolutions matching.
**Resolution:** (1) Removed forced `aspect-ratio: 16/9` from CSS UI container, letting `object-fit: contain` render native aspect. (2) Tuned `hls.js` initialization with `lowLatencyMode: true`, `liveSyncDurationCount: 2`, and `maxLiveSyncPlaybackRate: 2` to bypass the standard HLS 3-segment buffering delay.

### 22. Control Plane HTTP Segfault (Login Use-After-Free)
**Status:** ✅ RESOLVED
**Description:** The frontend hung endlessly on the login screen. The backend was crashing silently without returning HTTP 401.
**Resolution:** In `http_server.c`, `soup_buffer_free(body)` was being called *before* extracting the `username` and `password` strings from the JSON parser. Because JSON-GLib can use zero-copy string references back into the original buffer, the authentication string comparison (`token_auth_login`) was executing against deleted memory, throwing an instant segfault. Moved the free operation to the end of the handler.

### 23. Pipeline Teardown Deadlock / Segfault (AppSink Use-After-Free)
**Status:** ✅ RESOLVED
**Description:** The engine reliably crashed returning `Segmentation fault (core dumped)` exactly 5ms after an idle HLS branch was removed.
**Resolution:** The multithreaded teardown was freeing the `ManagedStream` C data structure before the GStreamer `appsink` was completely deactivated. The `new-sample` callback subsequently fired from the streaming thread on the newly-freed `ManagedStream` pointer. Solved by placing synchronous blocking API calls (`gst_element_set_state(appsink, GST_STATE_NULL)` and `emit-signals=FALSE`) on the Main Thread to explicitly disable callbacks *before* calling `g_free()`.

### 24. Javascript SyntaxError (Single-line Comment Concatenation)
**Status:** ✅ RESOLVED (2026-04-02)
**Root Cause:** C string literals in `web_assets.h` containing `//` comments were being concatenated into a single line by the browser. This caused the entire script after the first comment to be ignored, breaking `doLogin` and the button logic.
**Resolution:** Refactored `web_assets.h` to include explicit `\n` newlines and converted all JS comments to block format (`/* ... */`).

### 25. Static Recording Download Authentication Bypass
**Status:** ✅ RESOLVED (2026-04-02)
**Root Cause:** The `/recordings/` static file handler lacked an `http_server_authorize` call, allowing direct MP4 downloads without a token.
**Resolution:** Added mandatory `viewer` role verification to `_handle_static_recordings`.

### 26. Over-privileged "Viewer" Role for Lens Control
**Status:** ✅ RESOLVED (2026-04-02)
**Root Cause:** `_handle_lenses` incorrectly used the `viewer` role for authorization.
**Resolution:** Elevated the required role to `operator` for all state-changing lens operations.

### 27. Green Line Artifact (Bottom of Video)
**Status:** 🔶 DEFERRED (2026-04-03)
**Symptom:** A green line appears at the bottom of 1080p frames, often triggered or resolved by RTSP connect/disconnect.
**Root Cause:** Mod16 alignment mismatch (1080 vs 1088 preference in G2D) and dynamic buffer pool pollution on the capture tee.
**Resolution:** Attempted resolution via 1072p hardware scaling and firewall queues. Reverted to baseline for further manual testing of buffer pool settings.

### 28. GstShmSink Teardown Warning (Errno 16)
**Status:** 🔶 OPEN
**Symptom:** `gstshmsink` logs `errno: 16 (Device or resource busy)` when an AI model is stopped and the SHM branch is detached.
**Next Steps:** Investigate `shmsink` wait-for-connection settings and buffer pool release timing during dynamic pad removal.

### 29. RTSP API Compatibility (`gst_rtsp_mount_points_match`)
**Status:** ✅ RESOLVED (2026-04-03)
**Root Cause:** Used deprecated or non-existent `gst_rtsp_mount_points_match_factory` in diagnostic loops.
**Resolution:** Migrated to the correct `gst_rtsp_mount_points_match()` API in `main.c`.

### 30. D-Bus JSON Use-After-Free (UTF-8 Warning)
**Status:** ✅ RESOLVED (2026-04-03)
**Root Cause:** JSON parser unrefed before building output strings, causing dangling pointers to garbage memory.
**Resolution:** Corrected parser lifecycle in `main.c` to protect memory until response dispatch is complete.

### 31. AI Engine Serialization Crash (`np.float64`)
**Status:** ✅ RESOLVED (2026-04-03)
**Root Cause:** Python `json.dumps` fails to serialize NumPy types returned by the tracker.
**Resolution:** Injected type-casting logic in `ai_utils_*.py` to ensure all coordinates/confidence scores are standard floats before transmission.

### 32. Individual AI Overlay Toggle Log Error
**Status:** OPEN (Reported 2026-04-04)
**Severity:** HIGH
**Description:** Toggling the AI overlay for individual branches (e.g., toggling 'sub' but not 'main') is failing with a log error. Internal element QData may not be propagating correctly during dynamic branch attachment.

### 33. Third Branch (MJPEG) Connection Failure
**Status:** OPEN (Reported 2026-04-04)
**Severity:** HIGH
**Description:** The 'third' stream (plain 360p MJPEG) allows the pipeline to build but rejects RTSP connections. 
**Next Steps:** Verify the YUY2 ? MJPEG conversion path and the RTSP factory caps for the new direct-routing architecture.

### 34. Dynamic & Rebuild Configuration Updates Not Applied
**Status:** OPEN (Reported 2026-04-07)
**Severity:** CRITICAL
**Description:** Changes to Resolution, FPS, or Bitrate via API/CLI are acknowledged by the Control Plane but not physically applied to the GStreamer pipeline.
**Symptoms:** 
- **Dynamic**: Modifying params on a live stream has no visual effect.
- **Rebuild**: Stopping and restarting a branch (or reconnecting an RTSP client) continues to use legacy settings.
**Hypothesis:** 
- **UID Mismatch**: The engine may be failing to find the correct `capsfilter` elements by their UID string after the 최근 refactor.
- **RTSP Factory Caching**: `CustomRTSPFactory` may be holding onto stale pipeline descriptions.

### 35. Performance Bottleneck: `cairooverlay` High CPU Usage
**Status:** OPEN (Reported 2026-04-07)
**Severity:** HIGH
**Description:** Enabling AI overlays or clock overlays causes massive CPU spikes, leading to frame drops and UI sluggishness.
**Findings:** 
- Cairo operates in software on `BGRx` buffers. Rendering text and boxes on 1080p/30fps frames exceeds the CPU's thermal/processing budget on the i.MX8MP.
- The `_on_cairo_draw` callback fires at the full stream frame rate even when drawing is logically disabled, adding significant overhead.

### 36. Baseline CPU Usage Regression
**Status:** OPEN (Reported 2026-04-07)
**Severity:** MEDIUM
**Description:** The system baseline CPU usage is ~20-30% higher than historical benchmarks, even with all overlays disabled.
**Possible Causes:** 
- **Global Converters**: Mandatory NV12 → BGRx conversion path in `sys_capture.c` runs on every frame to support potential overlays.
- **Videorate overhead**: Redundant duplication of frames in recording/streaming branches.
- **SHM bandwidth**: Memcpy operations for AI SHM sink are consuming significant bus bandwidth even when AI is idle.

---

---

#### Iteration 1: Isolated Pipeline with AppSink→AppSrc Bridge (FAILED at 1080p)

**Architecture:** `appsink` on the main pipeline taps frames → `appsrc` pushes them into a separate `GstPipeline` for recording.

**Results:**
- ✅ 720p @ 10fps H.264 — worked
- ✅ 720p @ 20fps H.264 — worked
- ❌ 1080p @ 30fps H.264 — **deadlocked at frame 6**, RTSP froze

**Root Cause:** `gst_buffer_copy()` (shallow copy) takes a DMA reference to the hardware buffer. The VPU pool holds only 3–4 contiguous DMA blocks. `appsrc` hoarded these references, starving the silicon pool within 6 frames — the pipeline waited infinitely for a buffer that would never be recycled.

**Learning:** `appsink→appsrc` bridges with shallow-copy are fundamentally incompatible with large hardware-allocated buffers (1080p BGRx = 8.29 MB/frame).

---

#### Iteration 2: Direct TEE Architecture with BGRx (SEMI-SUCCESS)

**Architecture Change:** Eliminated the `appsink→appsrc` bridge. Recording elements grafted directly onto the main pipeline `tee` — exactly as the production engine does.

```
v4l2src → capsfilter(NV12) → G2D → capsfilter(BGRx) → tee
  tee → queue → G2D(scale) → capsfilter(WxH) → videorate → encoder
      → identity(drop-allocation) → parse → queue → splitmuxsink
```

**Results:**

| Resolution | FPS | Codec | Result |
|---|---|---|---|
| 1280×720 | 10 | h264 | ✅ Branch linked, RTSP stable, clean start/stop |
| 1280×720 | 20 | h264 | ✅ Multiple cycles successful |
| 1920×1080 | 30 | h264 | ❌ RTSP froze (CMA exhaustion) |
| 1920×1080 | 10 | h264 | ❌ Even low FPS failed at 1080p |
| 1280×720 | 10 | h264 | ✅ After 1080p failure, 720p still recovers |

**Root Cause of 1080p Failure — CMA Exhaustion:**

A single 1080p BGRx frame = `1920 × 1080 × 4 = 8.29 MB`. The VPU encoder needs 6 buffer slots minimum per instance = ~50MB per 1080p encoder. Two simultaneous 1080p encoders (RTSP + recording) require **~100 MB of contiguous physical RAM**, which exceeds the i.MX8MP's default CMA partition.

| Format | Frame Size (1080p) | Encoder Pool (6 slots) | Two Encoders |
|---|---|---|---|
| BGRx (32bpp) | 8.29 MB | ~50 MB | ~100 MB ❌ |
| NV12 (12bpp) | 3.11 MB | ~18 MB | ~36 MB ✅ |

**Root Cause of Empty `/tmp` (no files saved at ANY resolution):** The recording branch capsfilter forced `format=BGRx,colorimetry=1:1:16:4`. The branch `imxvideoconvert_g2d` could not negotiate BGRx→BGRx scaling — it is a format *converter*, not a passthrough scaler. Negotiation failed silently: no error, no frames, no files.

**Learnings:**
1. CMA is a hard **kernel-level physical memory ceiling** — no queue buffering or pipeline restructuring can overcome it.
2. `imxvideoconvert_g2d` **cannot do same-format passthrough** (BGRx→BGRx or NV12→NV12). It must always convert between formats.
3. Explicit `format=` in capsfilters can silently kill branches when colorimetry negotiation fails.

---

#### Iteration 3: Remove Explicit Format from Capsfilter (PARTIAL FIX)

**Change:** Removed `format=BGRx,colorimetry=1:1:16:4` from the recording capsfilter → `video/x-raw,width=%d,height=%d` only.

**Result:** Branch no longer died silently at 720p (auto-negotiation worked). 1080p still deadlocked (CMA unchanged).

**Learning:** Letting GStreamer auto-negotiate format between branch G2D and encoder is essential — the encoder and scaler agree on optimal format without colorimetry conflicts.

---

#### Iteration 4: Full NV12 Baseline (SUCCESS for all resolutions)

**Architecture Change:** Removed the main-pipeline G2D entirely. Camera outputs NV12 natively → flows directly to `tee`:

```
v4l2src → capsfilter(NV12,1920x1080,30fps) → tee (no G2D!)
  tee → queue → G2D(scale) → capsfilter(WxH) → videorate → encoder
      → identity(drop-allocation) → parse → queue → splitmuxsink
```

**Key Discovery:** `imxvideoconvert_g2d` CANNOT do NV12→NV12 passthrough. Attempting it causes an immediate `not-linked` pipeline failure and `v4l2src` shutdown. Since the camera already outputs NV12, the G2D in the main pipeline was entirely unnecessary — it only existed to convert NV12→BGRx, wasting CMA memory.

**Result:** Pipeline builds and links successfully. NV12 reduces CMA per encoder from 50 MB → 18 MB, making dual 1080p encoders physically possible. **Pending final board verification of file output.**

**Learning:** When source is NV12 and sink is a hardware H.264 encoder (accepts NV12 natively), the main-pipeline G2D is unnecessary overhead that wastes 62% of CMA memory.

---

#### Open Decision: BGRx vs NV12 for Production

> ⚠️ **The entire production system currently operates on BGRx.** RTSP, AI inference, snapshots, and Wayland display all receive BGRx from the capture tee. Switching to NV12 requires verifying:
> - RTSP encoders: `v4l2h264enc` accepts NV12 natively ✅
> - Wayland display: accepts NV12 ✅
> - Snapshot pipeline: `videoconvert` already present ✅
> - AI inference: may expect BGR/RGB input — **needs verification** ⚠️
> - Color accuracy: BGRx+colorimetry was chosen to prevent R/B channel swap on G2D. NV12 has no discrete R/G/B channels to swap ✅

#### Summary of Key Learnings

1. **`appsink→appsrc` shallow-copy causes DMA pool starvation** for large hardware buffers within frames, not seconds.
2. **BGRx is 2.67× more expensive than NV12** in CMA memory — the difference between dual 1080p working or not.
3. **`imxvideoconvert_g2d` is a format converter, NOT a passthrough** — cannot do NV12→NV12 or BGRx→BGRx.
4. **Explicit `format=` in capsfilters can silently kill branches** when colorimetry negotiation fails upstream.
5. **The `splitmuxsink` firewall pattern** (`identity(drop-allocation=true)` + `send-keyframe-requests=false`) **is proven stable** across all resolutions and codecs.
6. **CMA is a hard kernel ceiling** — only reducible by lowering per-frame memory (NV12) or increasing kernel CMA partition size.

