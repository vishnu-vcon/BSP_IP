#!/bin/bash
set -e

# Always run from the project root
cd "$(dirname "$0")/.."

# Use CC environment variable if set (useful for cross-compiling), otherwise default to gcc
COMPILER=${CC:-gcc}
CFLAGS="-Wall -Wextra -O2 -g -Isrc -Isrc/engine -Isrc/engine/gst_blocks -Isrc/engine/subsystems -Isrc/control_plane -Isrc/control_plane/core"
LDFLAGS=""

if [ "$USE_TSAN" == "1" ]; then
    echo "!!! ENABLING THREAD SANITIZER (TSAN) !!!"
    CFLAGS="-Wall -Wextra -O1 -g -fsanitize=thread -Isrc -Isrc/engine -Isrc/engine/gst_blocks -Isrc/engine/subsystems -Isrc/control_plane -Isrc/control_plane/core"
    LDFLAGS="-fsanitize=thread"
fi

echo "=========================================="
echo " Building SMART IP Edge Core Engine..."
echo "=========================================="
$COMPILER $CFLAGS -o smartip_engine \
    src/engine/main.c \
    src/engine/subsystems/sys_capture.c \
    src/engine/subsystems/sys_config.c \
    src/engine/gst_blocks/encoder_builder.c \
    src/engine/subsystems/sys_rtsp.c \
    src/engine/subsystems/sys_stream.c \
    src/engine/subsystems/sys_hls.c \
    src/engine/subsystems/sys_recording.c \
    src/engine/subsystems/sys_ntp.c \
    src/engine/api/dbus_handlers.c \
    src/common/event_broker.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0 gio-2.0 glib-2.0 json-glib-1.0 cairo) \
    -lzmq -lpthread
echo "✓ smartip_engine built"

echo "=========================================="
echo " Building Control Plane..."
echo "=========================================="
$COMPILER $CFLAGS -o smartip_control \
    src/control_plane/main.c \
    src/control_plane/core/auth.c \
    src/control_plane/core/http_server.c \
    src/control_plane/auth/db_manager.c \
    src/control_plane/auth/password_hash.c \
    src/control_plane/auth/password_policy.c \
    src/control_plane/auth/otp_manager.c \
    src/control_plane/auth/smtp_sender.c \
    src/control_plane/auth/activity_logger.c \
    src/control_plane/auth/role_manager.c \
    src/control_plane/auth/session_manager.c \
    src/control_plane/device_controls.c \
    src/common/event_broker.c \
    $(pkg-config --cflags --libs gio-2.0 glib-2.0 json-glib-1.0 libsoup-2.4 sqlite3 openssl libqrencode libpng libcurl) \
    -lzmq -lpthread -lcrypt
echo "✓ smartip_control built"

echo "=========================================="
echo " Building Alert Manager..."
echo "=========================================="
$COMPILER $CFLAGS -o smartip_alerts \
    src/alert_manager/main.c \
    src/common/event_broker.c \
    $(pkg-config --cflags --libs glib-2.0 json-glib-1.0) \
    -lzmq
echo "✓ smartip_alerts built"

echo "=========================================="
echo " Building CLI Tool..."
echo "=========================================="
$COMPILER $CFLAGS -o smartip_cli \
    src/cli/main.c \
    $(pkg-config --cflags --libs gio-2.0 glib-2.0 json-glib-1.0 libsoup-2.4) \
    -lpthread
echo "✓ smartip_cli built"

echo "=========================================="
echo " Building HLS Test Server..."
echo "=========================================="
$COMPILER $CFLAGS -o smartip_hls_test \
    src/tools/hls_test_server.c \
    $(pkg-config --cflags --libs libsoup-2.4)
echo "✓ smartip_hls_test built"

echo "=========================================="
echo " ALL BINARIES COMPILED SUCCESSFULLY!"
echo "=========================================="
