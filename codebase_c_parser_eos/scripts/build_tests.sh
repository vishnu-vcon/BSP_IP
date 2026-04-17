#!/bin/bash
set -e

# Always run from the project root
cd "$(dirname "$0")/.."

# Use CC environment variable if set, otherwise default to gcc
COMPILER=${CC:-gcc}
CFLAGS="-Wall -Wextra -O2 -g -Isrc -Isrc/control_plane -Isrc/control_plane/core -Isrc/engine -Isrc/engine/gst_blocks -Itests/mocks -DUNIT_TESTING"

echo "=========================================="
echo " Building Verification Tests (Mocked)..."
echo "=========================================="

# 1. Verify Auth Flow
echo "Building verify_auth_flow..."
GLIB_FLAGS=$(pkg-config --cflags --libs glib-2.0 gobject-2.0)
CMOCKA_FLAGS=$(pkg-config --cflags --libs cmocka 2>/dev/null || echo "-DNO_CMOCKA")

$COMPILER $CFLAGS -o verify_auth_flow \
    tests/verify_auth_flow.c \
    src/control_plane/core/auth.c \
    tests/mocks/mock_stubs.c \
    $GLIB_FLAGS $CMOCKA_FLAGS \
    -Wl,--wrap=g_hmac_new \
    -Wl,--wrap=g_hmac_update \
    -Wl,--wrap=g_hmac_get_string \
    -Wl,--wrap=g_hmac_unref

# 2. Verify Pipeline Logic
echo "Building verify_pipeline_logic..."
$COMPILER $CFLAGS -o verify_pipeline_logic \
    tests/verify_pipeline_logic.c \
    src/engine/gst_blocks/encoder_builder.c \
    src/common/event_broker.c \
    tests/mocks/mock_stubs.c \
    $GLIB_FLAGS $CMOCKA_FLAGS \
    -Wl,--wrap=gst_element_factory_make

echo "=========================================="
echo " Running Verifications..."
echo "=========================================="

./verify_auth_flow
./verify_pipeline_logic

echo "=========================================="
echo " ALL TESTS PASSED SUCCESSFULLY!"
echo "=========================================="
