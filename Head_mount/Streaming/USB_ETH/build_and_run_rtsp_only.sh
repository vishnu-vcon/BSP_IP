#!/bin/bash
set -e
echo "Compiling RTSP ONLY Server..."
gcc -Wall -Wextra -O3 -o sei_server_rtsp_only sei_server_rtsp_only.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-rtsp-server-1.0)
echo "Running RTSP ONLY Server..."
./sei_server_rtsp_only
