#!/bin/bash
# ============================================================
# CPU Isolation Test Pipelines for iMX8MP
# ============================================================
# Run each pipeline one at a time. In a second terminal, run:
#   top -d 1 -p $(pgrep -f gst-launch)
# or:
#   perf record -g -F 99 -p $(pgrep -f gst-launch) -- sleep 15
#   perf report --sort=symbol | head -30
#
# Each test runs for 20 seconds. Press Ctrl+C to stop early.
# ============================================================

DEVICE="/dev/video3"
CAPS="video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1"
BGRX_CAPS="video/x-raw,format=BGRx,width=1920,height=1080"
DURATION=20

run_test() {
    local name="$1"
    shift
    echo ""
    echo "============================================================"
    echo "TEST: $name"
    echo "============================================================"
    echo "Pipeline: gst-launch-1.0 $@"
    echo ""
    echo "Starting in 3s... Open 'top' in another terminal now."
    sleep 3
    timeout $DURATION gst-launch-1.0 -e "$@" 2>/dev/null
    echo ""
    echo "Test '$name' finished."
    echo "------------------------------------------------------------"
    sleep 2
}

echo "============================================================"
echo " SmartIP Edge CPU Isolation Tests"
echo " Device: $DEVICE"
echo "============================================================"
echo ""
echo "Run these tests ONE AT A TIME."
echo "Monitor CPU with: top -d 1"
echo "Or for detailed profiling: perf stat -p \$(pgrep gst-launch)"
echo ""

case "${1:-all}" in

# ── TEST 1: Baseline — Just capture, no processing ──
1|all)
run_test "1. BASELINE: V4L2 Capture → fakesink (expect ~2-5% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! fakesink sync=false
;;&

# ── TEST 2: Capture + G2D NV12→BGRx conversion ──
2|all)
run_test "2. G2D CONVERSION: V4L2 → G2D(NV12→BGRx) → fakesink (expect ~3-8% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! imxvideoconvert_g2d ! "$BGRX_CAPS" \
    ! fakesink sync=false
;;&

# ── TEST 3: Capture + G2D + Hardware H264 Encode ──
3|all)
run_test "3. HW ENCODE: V4L2 → G2D → v4l2h264enc → fakesink (expect ~5-10% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! imxvideoconvert_g2d ! "$BGRX_CAPS" \
    ! v4l2h264enc extra-controls="controls,video_bitrate=4000000,video_gop_size=30" \
    ! h264parse ! fakesink sync=false
;;&

# ── TEST 4: Full encode + appsink (simulates shared encoder output) ──
4|all)
run_test "4. APPSINK: V4L2 → G2D → encode → appsink drop=true (expect ~5-12% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! imxvideoconvert_g2d ! "$BGRX_CAPS" \
    ! v4l2h264enc extra-controls="controls,video_bitrate=4000000,video_gop_size=30" \
    ! h264parse \
    ! appsink name=sink emit-signals=true drop=true max-buffers=2 sync=false
;;&

# ── TEST 5: Tee fan-out (2 copies of encoded stream — simulates RTSP+HLS) ──
5|all)
run_test "5. TEE FAN-OUT: encode → tee → 2x fakesink (expect ~8-15% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! imxvideoconvert_g2d ! "$BGRX_CAPS" \
    ! v4l2h264enc extra-controls="controls,video_bitrate=4000000,video_gop_size=30" \
    ! h264parse ! tee name=t \
    t. ! queue leaky=downstream max-size-buffers=2 ! fakesink sync=false \
    t. ! queue leaky=downstream max-size-buffers=2 ! fakesink sync=false
;;&

# ── TEST 6: Direct RTSP server (NO appsink/appsrc bridge — zero buffer copy) ──
6|all)
run_test "6. DIRECT RTSP: V4L2 → encode → rtph264pay → udpsink (expect ~5-12% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! imxvideoconvert_g2d ! "$BGRX_CAPS" \
    ! v4l2h264enc extra-controls="controls,video_bitrate=4000000,video_gop_size=30" \
    ! h264parse ! rtph264pay pt=96 config-interval=1 \
    ! udpsink host=127.0.0.1 port=5004 sync=false
;;&

# ── TEST 7: NV12 direct to encoder (recording path — no BGRx conversion) ──
7|all)
run_test "7. NV12 DIRECT ENCODE: V4L2(NV12) → v4l2h264enc → fakesink (expect ~3-8% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! v4l2h264enc extra-controls="controls,video_bitrate=4000000,video_gop_size=30" \
    ! h264parse ! fakesink sync=false
;;&

# ── TEST 8: NV12 with G2D scaling (recording path with resolution change) ──
8|all)
run_test "8. NV12 SCALED ENCODE: V4L2(NV12) → G2D(NV12→NV12@1280x720) → encode → fakesink" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! imxvideoconvert_g2d ! "video/x-raw,format=NV12,width=1280,height=720" \
    ! v4l2h264enc extra-controls="controls,video_bitrate=2000000,video_gop_size=25" \
    ! h264parse ! fakesink sync=false
;;&

# ── TEST 9: SHM write (AI path — raw frames to shared memory) ──
9|all)
run_test "9. SHM WRITE: V4L2(NV12) → shmsink (simulates AI SHM path, expect ~10-20% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! videorate drop-only=true max-rate=20 skip-to-first=true \
    ! shmsink socket-path=/tmp/test_shm_perf shm-size=67108864 wait-for-connection=false sync=false
;;&

# ── TEST 10: Full simulation — encode + tee + RTSP pay + SHM (combined load) ──
10|all)
run_test "10. COMBINED: capture → tee → [encode+RTSP] + [SHM@20fps] (expect ~15-25% CPU)" \
    v4l2src device=$DEVICE io-mode=dmabuf ! "$CAPS" \
    ! tee name=t allow-not-linked=true \
    t. ! queue leaky=downstream max-size-buffers=2 \
       ! imxvideoconvert_g2d ! "$BGRX_CAPS" \
       ! v4l2h264enc extra-controls="controls,video_bitrate=4000000,video_gop_size=30" \
       ! h264parse ! rtph264pay pt=96 config-interval=1 \
       ! udpsink host=127.0.0.1 port=5004 sync=false \
    t. ! queue leaky=downstream max-size-buffers=2 \
       ! videorate drop-only=true max-rate=20 skip-to-first=true \
       ! shmsink socket-path=/tmp/test_shm_perf shm-size=67108864 wait-for-connection=false sync=false
;;&

esac

# Cleanup
rm -f /tmp/test_shm_perf 2>/dev/null

echo ""
echo "============================================================"
echo " All tests complete."
echo ""
echo " KEY INSIGHT: If tests 3-6 show low CPU (~10-15%), but your"
echo " SmartIP engine shows 60%+, the bottleneck is in the"
echo " appsink → gst_buffer_copy → appsrc bridge code, NOT in"
echo " the GStreamer pipeline itself."
echo ""
echo " The fix (already applied) replaces gst_buffer_copy() with"
echo " gst_buffer_ref + gst_buffer_make_writable (zero-copy)."
echo "============================================================"
