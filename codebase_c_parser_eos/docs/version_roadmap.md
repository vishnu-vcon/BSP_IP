# Version Roadmap & Planned Features

This document inherits the legacy version plans and adapts them to the new C Engine architecture.

---

## 1. Engine Hardware Optimization

### Shared Encoder Session (RTSP + Recording)
**Current:** RTSP and Recording branches create independent hardware encoder elements from the `capture_tee`.
**Goal:** Share a single hardware encoder output via a second tee *after* the encoder.
**Blocker:** Dynamic parameters. If a viewer changes live RTSP FPS/Resolution, the Recording would be physically damaged. 
**Status:** DELAYED. Separation of concerns (RTSP mutability vs Recording stability) is currently more critical than VPU optimization.

### G2D Hardware Scaler Profiling
**Current:** Reusing the `imxvideoconvert_g2d` scaler in the branches.
**Goal:** Verify whether G2D dynamically renegotiates DMA pools when downstream elements (like `videorate`) change state. If it doesn't, we need to inject structural caps filters dynamically.

---

## 2. Platform Architecture Enhancements

### HTTP Streaming Port Separation
**Background:** Industry standard ONVIF/VAPIX cameras physically separate their control planes (HTTPS) from their streaming planes. 
**Plan:** 
- Keep `port 8443 (HTTPS)` locked to the Control Plane API (commands, status, auth).
- Push direct MJPEG browser hooks to `port 8080 (HTTP)`. We need to add simple `?token=` query param interceptors directly to the C Engine's HTTP server to mimic the safety of the Control Plane without proxying heavy JPEGs through D-Bus or sockets.

### SSE Alerts Stream
**Plan:** Port the chunked `text/event-stream` response handler from the python codebase into the C `http_server.c`. This will allow the browser UI to listen to ZMQ alerts in real time over HTTP/2.

---

## 3. Artificial Intelligence Resurgence (Phase 8)

### C-Native AI Branching
**Background:** The `AIBranch` was intentionally ripped out of the C codebase to drop the 148 MB Python overhead and stop the Cairo overlay CPU spike.
**Plan:** Integrate `libtensorflowlite_c.so` natively into a new GStreamer pad probe. We will execute the tensor frames in C, extract the bounding boxes, and broadcast them entirely via ZMQ *without* burning them into the video frames via `cairooverlay`. The web client UI will draw the boxes natively in HTML5/React, keeping Edge CPU at absolute zero for rendering!
