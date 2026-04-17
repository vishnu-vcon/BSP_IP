#!/bin/bash

# Build script for Cairooverlay Test Application
# Usage: ./build_cairo_test.sh

echo "Building Cairooverlay Test Application..."

# Compiler and flags
COMPILER=${CC:-gcc}
CFLAGS="-Wall -Wextra -O2 -g"

echo "Using compiler: $COMPILER"

# Source and target
SRC="test_cairo_overlay.c"
TARGET="test_cairo_overlay"

# Build flags
GST_FLAGS=$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 cairo)

if [ $? -ne 0 ]; then
    echo "Error: pkg-config failed. Make sure gstreamer-1.0 and cairo are installed."
    exit 1
fi

# Compile
echo "Compiling $TARGET..."
$COMPILER $CFLAGS -o $TARGET $SRC $GST_FLAGS

if [ $? -eq 0 ]; then
    echo "✓ Successfully built $TARGET"
    echo "To run: ./$TARGET"
else
    echo "Build failed."
    exit 1
fi
