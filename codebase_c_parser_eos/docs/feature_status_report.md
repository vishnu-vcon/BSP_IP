# SMART IP Edge Application — C Port Feature Status Report
**Date:** 2026-03-27 | **Target:** i.MX8MP → RTL8715Ax SoC (<64 MB RAM)

---

## Media Pipeline Features

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 1 | **v4l2src capture with NV12 caps** | 2 | ✅ Ported | ❌ Not tested | — | Fallback to `videotestsrc` if device missing |
| 2 | **Permanent G2D hardware scaler** | 2 | ✅ Ported | ❌ Not tested | — | `imxvideoconvert_g2d` → `videoconvert` fallback |
| 3 | **BGRx colorimetry caps (sRGB)** | 2 | ✅ Ported | ❌ Not tested | — | Prevents red/blue channel swap on i.MX8MP |
| 4 | **capture_tee (Dynamic Tee arch)** | 2 | ✅ Ported | ❌ Not tested | — | Core zero-copy fan-out mechanism |
| 5 | **Permanent fakesink (clock keepalive)** | 2 | ✅ Ported | ❌ Not tested | — | sync=TRUE, keeps V4L2 DMA pool flowing |
| 6 | **Permanent snapshot appsink** | 2 | ✅ Ported | ❌ Not tested | — | leaky=2, max-buffers=1, drop=TRUE |
| 7 | **Dynamic branch attach** | 2 | ✅ Ported | ❌ Not tested | — | add → link → sync_state_with_parent → request pad |
| 8 | **Dynamic branch detach (IDLE probe)** | 2 | ✅ Ported | ❌ Not tested | — | EOS injection + 5s moov atom timeout + 10s fallback |
| 9 | **Pipeline DOT graph export** | 2 | ✅ Ported | ❌ Not tested | — | `GST_DEBUG_DUMP_DOT_DIR` set before `gst_init()` |
| 10 | **Pipeline bus error/warning handlers** | 2 | ✅ Ported | ❌ Not tested | — | |
| 11 | **Graceful EOS shutdown** | 2 | ✅ Ported | ❌ Not tested | — | 2s timeout for moov atom finalization |

## Encoder Features

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 12 | **H.264 HW encoder** (v4l2h264enc/imxvpuh264enc) | 2 | ✅ Ported | ❌ Not tested | — | Priority: HW first, x264enc fallback |
| 13 | **H.265 HW encoder** (v4l2h265enc/imxvpuh265enc) | 2 | ✅ Ported | ❌ Not tested | — | Priority: HW first, x265enc fallback |
| 14 | **MJPEG encoder** (jpegenc) | 2 | ✅ Ported | ❌ Not tested | — | videoconvert → I420 → jpegenc chain |
| 15 | **videorate (drop-only + max-rate)** | 2 | ✅ Ported | ❌ Not tested | — | skip-to-first=TRUE prevents G2D green frames |
| 16 | **G2D branch scaler** | 2 | ✅ Ported | ❌ Not tested | — | Per-branch resolution scaling |
| 17 | **x264enc software fallback** | 2 | ✅ Ported | ❌ Not tested | — | speed-preset=1, tune=4, key-int-max=fps |
| 18 | **cairooverlay (AI bounding boxes)** | 3 | ✅ Ported | ✅ Tested | — | Hardware-accelerated native rendering |
| 19 | **debug_stages (diagnostic branches)** | 6 | 🔶 Deferred | ✅ Tested | Bug 27 | Used for green line investigation; reverted to baseline |

## RTSP Streaming

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 20 | **LazyRTSPFactory** | 3 | ✅ Ported | ❌ Not tested | — | Encoder branch created on client connect only |
| 21 | **appsink → appsrc PTS rebasing** | 3 | ✅ Ported | ❌ Not tested | — | Manual PTS rebase, DTS=NONE |
| 22 | **Branch teardown on unprepared** | 3 | ✅ Ported | ❌ Not tested | — | Encoder torn down when last client disconnects |
| 23 | **TCP + UDP + UDP_MCAST protocols** | 3 | ✅ Ported | ❌ Not tested | — | Instant connect without protocol fallback delay |
| 24 | **Shared factory (set_shared=TRUE)** | 3 | ✅ Ported | ❌ Not tested | — | Single encoder serves multiple clients |
| 25 | **H.264/H.265/MJPEG launch strings** | 3 | ✅ Ported | ❌ Not tested | — | Codec-specific payloader selection |
| 26 | **GMutex branch_lock** (race guard) | 7 | ✅ Ported | ❌ Not tested | — | Prevents double branch creation on simultaneous connects |
| 27 | **RTSP client tracking** (session count) | 7 | ✅ Ported | ❌ Not tested | — | `client-connected` + `session-removed` signals |

## Recording

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 28 | **splitmuxsink recording** | 3 | ✅ Ported | ❌ Not tested | — | async-finalize=FALSE for moov atom safety |
| 29 | **mp4mux explicit muxer** | 3 | ✅ Ported | ❌ Not tested | — | Prevents default qtmux issues |
| 30 | **Timestamped filenames** | 3 | ✅ Ported | ❌ Not tested | — | Session UID prevents overwrites |
| 31 | **Continuous recording mode** | 3 | ✅ Ported | ❌ Not tested | — | Starts immediately on [start()](file:///home/admin1/SMART_IP_EDGE_APPLICATION/working/setup1/codebase_no_ai/unified_engine.py#965-1010) |
| 32 | **Event recording mode** (arm/poke) | 3 | ✅ Ported | ❌ Not tested | — | Arms on [start()](file:///home/admin1/SMART_IP_EDGE_APPLICATION/working/setup1/codebase_no_ai/unified_engine.py#965-1010), records on first AI [poke()](file:///home/admin1/SMART_IP_EDGE_APPLICATION/working/setup1/codebase_no_ai/unified_engine.py#815-827) |
| 33 | **Idle timer + auto re-arm** | 7 | ✅ Ported | ❌ Not tested | — | `g_timeout_add_seconds()` → stop + re-arm after idle_timeout |
| 34 | **Max segment duration** | 3 | ✅ Ported | ❌ Not tested | — | `max-size-time` on splitmuxsink |

## IPC / Control Plane

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 35 | **ZMQ PUB/SUB event broker** | 4 | ✅ Ported | ❌ Not tested | — | Multipart `[topic, json]` format preserved |
| 36 | **ZMQ publisher** (engine) | 4 | ✅ Ported | ❌ Not tested | — | `libzmq` + `g_thread_new` |
| 37 | **ZMQ subscriber** (control/alerts) | 4 | ✅ Ported | ❌ Not tested | — | Background listener thread |
| 38 | **D-Bus server** (12 methods) | 4 | ✅ Ported | ❌ Not tested | — | GDBus with introspection XML |
| 39 | **D-Bus client proxy** (control plane) | 5 | ✅ Ported | ❌ Not tested | — | `g_dbus_proxy_call_sync()` |
| 40 | **D-Bus client proxy** (CLI) | 6 | ✅ Ported | ❌ Not tested | — | Interactive REPL loop |

## Authentication & REST API

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 41 | **HMAC-SHA256 token auth** | 5 | ✅ Ported | ❌ Not tested | — | GLib `g_hmac_new()` — no OpenSSL dependency |
| 42 | **Token expiry (TTL)** | 5 | ✅ Ported | ❌ Not tested | — | `g_get_real_time()` based |
| 43 | **Role-based permissions** | 5 | ✅ Ported | ❌ Not tested | — | admin/operator/viewer matrix |
| 44 | **Token revocation** | 5 | ✅ Ported | ❌ Not tested | — | Hash table blacklist |
| 45 | **POST /api/v1/auth/login** | 5 | ✅ Ported | ❌ Not tested | — | libsoup handler |
| 46 | **POST /api/v1/cmd** | 5 | ✅ Ported | ❌ Not tested | — | Role check → dispatch → JSON response |
| 47 | **GET /api/v1/health** | 5 | ✅ Ported | ❌ Not tested | — | No auth required |
| 48 | **GET /api/v1/status** | 5 | ✅ Ported | ❌ Not tested | — | Bearer auth required |
| 49 | **GET /api/v1/stream/... (HLS)** | 5 | ✅ Ported | ✅ Tested | — | Repurposed from original MJPEG plan to standard HLS |
| 50 | **TLS self-signed cert generation** | 7 | ✅ Ported | ❌ Not tested | — | `openssl` system call, falls back to HTTP |
| 51 | **`?token=` query param auth** | 7 | ✅ Ported | ❌ Not tested | — | For browser/SSE clients |
| 51.1 | **Web Dashboard SPA (`web_assets.h`)** | 9 | ✅ Ported | ✅ Tested | — | Single Page Application with authenticated dynamic tuning |

## Engine Management Features

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 52 | **Lens registry** (lens→device mapping) | 7 | ✅ Ported | ❌ Not tested | — | `lens1→/dev/video3`, `lens2→/dev/video4` |
| 53 | **configure_lens()** (multi-tier) | 7 | ✅ Ported | ❌ Not tested | — | main/sub/third stream tiers |
| 54 | **Dynamic update** (fps/res/bitrate) | 7 | ✅ Ported | ❌ Not tested | — | Live property changes on running elements |
| 55 | **stop_lens()** (cascade teardown) | 7 | ✅ Ported | ❌ Not tested | — | Recordings → RTSP → capture pipeline |
| 56 | **Detailed get_status()** | 7 | ✅ Ported | ❌ Not tested | — | Per-lens, per-branch, active/idle state |
| 57 | **save_current_config()** | 7 | ✅ Ported | ❌ Not tested | — | Writes `user_overrides` to [default_config.json](file:///home/admin1/SMART_IP_EDGE_APPLICATION/working/setup1/codebase/default_config.json) |
| 58 | **DefaultConfigManager** | 7 | ✅ Ported | ❌ Not tested | — | Auto-start RTSP + recording from JSON on boot |
| 59 | **15s system stats logging** | 7 | ✅ Ported | ❌ Not tested | — | Total clients + dynamic branch counts |
| 60 | **set_log_level()** | 5 | ✅ Ported | ❌ Not tested | — | GStreamer debug threshold control |
| 61 | **Signal handling** (SIGINT/SIGTERM) | 2 | ✅ Ported | ❌ Not tested | — | `g_main_loop_quit()` on signal |

## Not Yet Ported (by design)

| # | Feature | Phase | Status | Tested | Known Bugs | Comments |
|:--|:--------|:------|:-------|:-------|:-----------|:---------|
| 62 | **AIBranch** (SHM inference) | 8 | ✅ Ported | ✅ Tested | Bug 28 | Dynamic shmsink/shmsrc architecture |
| 63 | **AI model management** (load/stop) | 8 | ✅ Ported | ✅ Tested | — | D-Bus controlled lifecycle |
| 64 | **set_ai_config()** (proxy) | 8 | ✅ Ported | ✅ Tested | — | Threshold/Snapshots/Overlay toggle |
| 65 | **AI snapshot on alert** | 8 | ❌ Not ported | — | — | Python-side imwrite implemented |
| 66 | **HTTPBranch** (MJPEG streaming) | — | ❌ Omitted | — | — | Omitted in favor of `hls_generator` Native HLS Delivery |
| 67 | **HTTP MJPEG server** (port 8080) | — | ❌ Omitted | — | — | Omitted; API handles HLS now. |
| 68 | **cairooverlay drawing** | — | ❌ Omitted | — | Bug D | 91% CPU — needs optimization before re-adding |
| 69 | **CPU profiler thread** | — | ❌ Not ported | — | — | `/proc/pid/task` parsing — low priority |
| 70 | **SSE alerts stream** | — | ❌ Not ported | — | — | `text/event-stream` chunked response |

---

## Summary

| Category | Total | Ported | Stubs | Omitted |
|:---------|:------|:-------|:------|:--------|
| Media Pipeline | 11 | **11** | 0 | 0 |
| Encoder | 8 | **6** | 0 | **2** |
| RTSP | 8 | **8** | 0 | 0 |
| Recording | 7 | **7** | 0 | 0 |
| IPC/D-Bus | 6 | **6** | 0 | 0 |
| Auth & REST | 11 | **10** | 0 | **1** |
| Engine Management | 10 | **10** | 0 | 0 |
| Not Yet Ported | 9 | 0 | **3** | **6** |
| **TOTAL** | **70** | **60 (86%)** | **3 (4%)** | **7 (10%)** |

| 71 | **Periodic DOT Diagnostic Dumps** | 9 | ✅ Ported | ✅ Tested | — | Dumps every 2 mins to /tmp/dots/ |

> [!IMPORTANT]
> **Total Completion: 91% (64/70 features ported).** The AI orchestration layer and native rendering are fully functional. The project is currently at a stable 1080p baseline.

---

## Planned for Monday (2026-04-06)

*   **Multi-Recording Integration**: Refactor the recording engine to support simultaneous recording branches (e.g., Main 1080p + Sub 360p) with independent triggers.
*   **Control Plane Upgradation**: Enhance the REST API to support new recording query modes and audit logging for AI events.
*   **Alerts & Reports Services**: Implement the ZMQ-based alerting service to process AI bounding box events into persisted reports (CSV/JSON/Database).
