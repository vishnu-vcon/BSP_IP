#!/bin/bash
set -e
echo "Compiling NO PROPS Server..."
gcc -Wall -Wextra -O3 -o sei_server_no_props sei_server_no_props.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-rtsp-server-1.0)
echo "Running NO PROPS Server..."
./sei_server_no_props
