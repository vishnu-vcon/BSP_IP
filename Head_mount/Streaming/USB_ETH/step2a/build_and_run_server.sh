#!/bin/bash
echo "Compiling RTSP SEI Server App..."

# We added gstreamer-rtsp-server-1.0 to the pkg-config call here:
gcc sei_server.c -o sei_server $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-rtsp-server-1.0) -pthread

if [ $? -eq 0 ]; then
    echo "Build successful. Starting Server..."
    ./sei_server
else
    echo "Build failed. Check your GStreamer includes."
fi
