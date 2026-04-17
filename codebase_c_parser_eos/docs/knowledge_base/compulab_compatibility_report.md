# Compulab ucm-imx8m-plus Compatibility Report

## Platform Comparison

| Aspect | NXP EVK (Current) | Compulab ucm-imx8m-plus |
|--------|-------------------|------------------------|
| **SoC** | i.MX 8M Plus | i.MX 8M Plus (same) |
| **BSP** | `6.12-walnascar` | `6.6-scarthgap` |
| **Yocto** | Walnascar (latest) | Scarthgap (LTS) |
| **GStreamer** | **1.26.0** | **1.24.7** |
| **Kernel** | ~6.12.x | ~6.6.x |
| **Distro** | fsl-imx-xwayland | fsl-imx-xwayland (same) |

## Verdict: ✅ COMPATIBLE (with minor precautions)

GStreamer 1.24 and 1.26 are both part of the stable 1.x ABI series. There are **no breaking API changes** between them. All core APIs used by our application (`v4l2src`, `tee`, `queue`, `v4l2h264enc`, `imxvideoconvert_g2d`, `hlssink`, `appsink`, GstRTSPServer) exist and are stable across both versions.

---

## Detailed API Audit

### ✅ Safe — No Issues

| API / Element | Since Version | EVK (1.26) | Compulab (1.24) | Status |
|--------------|---------------|------------|-----------------|--------|
| `gst_element_request_pad_simple()` | **1.20** | ✅ | ✅ | Safe |
| `gst_rtsp_mount_points_match()` | **1.0** | ✅ | ✅ | Safe |
| `v4l2h264enc` / `v4l2h265enc` | BSP dependent | ✅ | ✅ | Same SoC, same VPU |
| `imxvideoconvert_g2d` | BSP dependent | ✅ | ✅ | Same SoC, same G2D |
| `hlssink` | **1.12** | ✅ | ✅ | Safe |
| `appsink` / `appsrc` | **1.0** | ✅ | ✅ | Safe |
| `g_object_class_find_property()` (runtime probe) | GLib 2.x | ✅ | ✅ | Safe |
| `splitmuxsink` | **1.6** | ✅ | ✅ | Safe |
| `shmsink` / `shmsrc` | **1.0** | ✅ | ✅ | Safe |
| `cairooverlay` | **1.0** | ✅ | ✅ | Safe |
| `libsoup-2.4` (Control Plane) | N/A | ✅ | ✅ Available in Scarthgap | Safe |

### ⚠️ Precautions Required

#### 1. `vbr-mode` Property (NXP VPU Patch)
The `vbr-mode` property on `v4l2h264enc` is an **NXP-specific kernel patch**, not a GStreamer upstream property. Its availability depends on the **kernel version** (6.6 vs 6.12), not GStreamer.

**Our code already handles this safely:**
```c
// main.c:1409 — Runtime probe before setting
if (g_object_class_find_property(G_OBJECT_GET_CLASS(active_branch->elements[i]), "vbr-mode")) {
    g_object_set(active_branch->elements[i], "vbr-mode", vbm, NULL);
} else {
    g_warning("Encoder has no 'vbr-mode' property (NXP patch not present)");
}
```
**Status:** ✅ Already protected by runtime detection. Will gracefully fall back.

#### 2. `extra-controls` GstStructure (VPU Encoder)
The `extra-controls` property on V4L2 encoders passes kernel-level V4L2 control IDs. Between kernel 6.6 and 6.12, NXP may have added or renamed specific control IDs (e.g., `h264_i_frame_period`, `video_bitrate`, `video_gop_size`).

**Risk:** LOW. These are standard V4L2 encoder controls. The kernel driver silently ignores unknown controls.

**Action:** Test on the Compulab to confirm VPU accepts all control IDs.

#### 3. `hlssink` vs `hlssink2` vs `hlssink3`
Our code uses `hlssink` (original). GStreamer 1.26 adds `hlssink3`, but we do NOT use it. Both 1.24 and 1.26 ship the original `hlssink`.

**Status:** ✅ No issue.

#### 4. V4L2 Device Node Paths
The NXP EVK uses `/dev/video3` and `/dev/video4` for camera capture. The Compulab board may assign **different device node numbers** depending on its device tree and sensor configuration.

**Action Required:** Verify device nodes on the Compulab board:
```bash
v4l2-ctl --list-devices
```

#### 5. CMA Allocation Size
The default CMA allocation may differ between the two BSP versions. Our pipeline requires at minimum 128MB CMA for stable multi-branch operation.

**Action Required:** Check the Compulab bootargs:
```bash
cat /proc/cmdline | grep cma
cat /proc/meminfo | grep Cma
```

---

## Build System Compatibility

### Meson Version
- Scarthgap ships **Meson ≥ 1.3.x** (GStreamer 1.26 requires Meson ≥ 1.4 for building GStreamer itself, but NOT for applications that depend on it).
- Our `meson.build` only requires basic Meson features. ✅ Safe.

### Library Dependencies on Scarthgap

| Dependency | Package | Scarthgap Status |
|-----------|---------|-----------------|
| `gstreamer-1.0` | gstreamer1.0 | ✅ Available (1.24.7) |
| `gstreamer-app-1.0` | gstreamer1.0-plugins-base | ✅ Available |
| `gstreamer-rtsp-server-1.0` | gstreamer1.0-rtsp-server | ✅ Available |
| `glib-2.0` | glib-2.0 | ✅ Available |
| `gio-2.0` | glib-2.0 | ✅ Available |
| `json-glib-1.0` | json-glib | ✅ Available |
| `libsoup-2.4` | libsoup-2.4 | ✅ Available (co-exists with libsoup-3) |
| `cairo` | cairo | ✅ Available |
| `sqlite3` | sqlite3 | ✅ Available |
| `openssl` | openssl | ✅ Available |
| `libzmq` | zeromq | ⚠️ May need `IMAGE_INSTALL:append` in Yocto |
| `libqrencode` | qrencode | ⚠️ May need recipe or `IMAGE_INSTALL:append` |
| `libpng` | libpng | ✅ Available |
| `libcurl` | curl | ✅ Available |
| `libcrypt` | libxcrypt | ✅ Available |

### Potentially Missing on Compulab Image
The following libraries are less common in minimal Yocto images and may need to be explicitly added:

```bash
# Check on the Compulab board:
pkg-config --exists libzmq && echo "ZMQ: OK" || echo "ZMQ: MISSING"
pkg-config --exists libqrencode && echo "QREncode: OK" || echo "QREncode: MISSING"
pkg-config --exists cmocka && echo "CMocka: OK" || echo "CMocka: MISSING"
```

If missing, add to your Yocto image recipe:
```
IMAGE_INSTALL:append = " zeromq qrencode cmocka"
```

---

## Kernel-Level Differences (6.6 vs 6.12)

| Feature | Kernel 6.6 (Compulab) | Kernel 6.12 (EVK) | Impact |
|---------|----------------------|-------------------|--------|
| ISI Driver | Present | Present | ✅ Same hardware |
| VPU (Hantro) Driver | Present | Present | ✅ Same encoder |
| G2D (Vivante) Driver | Present | Present | ✅ Same 2D GPU |
| V4L2 Buffer Pool Size | Likely 4-6 | Likely 4-6 | ✅ Same limits apply |
| `vbr-mode` patch | May differ | Present | ⚠️ Runtime-checked |

---

## Pre-Deployment Checklist for Compulab

Run these commands on the Compulab board before building:

```bash
# 1. Verify GStreamer version
gst-launch-1.0 --version

# 2. Verify V4L2 devices
v4l2-ctl --list-devices

# 3. Verify hardware elements exist
gst-inspect-1.0 imxvideoconvert_g2d
gst-inspect-1.0 v4l2h264enc
gst-inspect-1.0 v4l2h265enc

# 4. Check CMA allocation
cat /proc/meminfo | grep -i cma

# 5. Check all pkg-config dependencies
for dep in gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0 \
           glib-2.0 gio-2.0 json-glib-1.0 libsoup-2.4 cairo sqlite3 \
           openssl libzmq libqrencode libpng libcurl; do
    pkg-config --exists $dep 2>/dev/null && echo "$dep: OK" || echo "$dep: MISSING"
done

# 6. Verify VPU encoder properties
gst-inspect-1.0 v4l2h264enc | grep -i "vbr-mode\|extra-controls"
```
