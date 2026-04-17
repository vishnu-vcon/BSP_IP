#!/bin/bash
# run_app.sh — Unified SmartIP Edge Startup Script
# =================================================

# 1. Resolve Project Root (ensure script works from anywhere)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR/.." || exit 1
BASE_DIR=$(pwd)

# 2. Setup GStreamer Debugging (DOT Dumps)
echo "Setting up GStreamer DOT dump directory..."
export GST_DEBUG_DUMP_DOT_DIR=/data/dot
mkdir -p "$GST_DEBUG_DUMP_DOT_DIR"
mkdir -p /data/hls
rm -f "$GST_DEBUG_DUMP_DOT_DIR"/*.dot

# 3. Kill any existing instances
echo "Stopping existing services..."
pkill -9 smartip_engine
pkill -9 smartip_control
pkill -9 -f ai_engine.py

# 4. Start the AI Engine (Python) in the background
echo "Starting AI Engine..."
# AI Engine needs to be run from its directory to find models/configs if relative paths are used
cd "$BASE_DIR/src/ai_engine" || exit 1
python3 ai_engine.py 2>&1 | sed -e 's/^/[AI] /' | tee /tmp/ai_engine.log &
AI_PID=$!
cd "$BASE_DIR" || exit 1

# 5. Start the Unified Engine (C)
echo "Starting Unified Engine (Capture & RTSP)..."
./smartip_engine 2>&1 | sed -e 's/^/[ENG] /' | tee /tmp/engine.log &
ENG_PID=$!

# 6. Start the Control Plane (REST API)
echo "Starting Control Plane..."
./smartip_control 2>&1 | sed -e 's/^/[CTL] /' | tee /tmp/control.log &
CTL_PID=$!

echo "==========================================="
echo " SMART IP Edge System Started Successfully "
echo "==========================================="
echo " Logs available in /tmp/ and streaming below "
echo " Use 'python3 src/cli/smartip_cli.py' in another terminal to interact."
echo " Press Ctrl+C to stop all services."
echo "==========================================="

trap "echo 'Stopping services...'; kill $AI_PID $ENG_PID $CTL_PID; exit 1" SIGINT SIGTERM
wait
