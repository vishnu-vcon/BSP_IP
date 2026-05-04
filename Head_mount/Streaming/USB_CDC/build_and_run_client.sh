#!/bin/bash
echo "Compiling SEI Client App..."
gcc sei_client.c -o sei_client $(pkg-config --cflags --libs gstreamer-1.0)

if [ $? -eq 0 ]; then
    echo "Build successful. Listening for stream..."
    ./sei_client
else
    echo "Build failed. Check your GStreamer includes."
fi
