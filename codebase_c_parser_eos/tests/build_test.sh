#!/bin/bash
# build_test.sh — Compile the High-Fidelity tester
# Usage: ./build_test.sh

echo "Compiling High-Fidelity Zero-Copy Tester..."

gcc tests/zero_copy_tester.c -o tests/zero_copy_tester \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0 glib-2.0 cairo) \
    -lpthread -lm

if [ $? -eq 0 ]; then
    chmod +x tests/zero_copy_tester
    echo "----------------------------------------------------"
    echo "BUILD SUCCESSFUL: ./tests/zero_copy_tester"
    echo "----------------------------------------------------"
    echo "RUN BASELINE: ./tests/zero_copy_tester /dev/video3,/dev/video4 2"
    echo "RUN OPTIMIZED: ./tests/zero_copy_tester /dev/video3,/dev/video4 4"
    echo "----------------------------------------------------"
else
    echo "BUILD FAILED. Ensure gstreamer1.0-plugins-base-devel and libcairo2-dev are installed."
fi
