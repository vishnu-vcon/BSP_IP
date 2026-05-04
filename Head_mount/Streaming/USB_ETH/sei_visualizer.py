import subprocess
import time
import re
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from mpl_toolkits.mplot3d import Axes3D
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
from math import atan2, sqrt, degrees
import numpy as np
import threading
import sys

# Regex to parse the single line output of sei_client:
# Example: SEI #20 | A(-160,-80,-970)mg G(937,-625,-312)mdps M(1,11,25)uT | Latency: 200 us
REGEX = re.compile(
    r"A\((-?\d+),(-?\d+),(-?\d+)\)mg G\((-?\d+),(-?\d+),(-?\d+)\)mdps M\((-?\d+),(-?\d+),(-?\d+)\)uT"
)

def parse_line(line):
    match = REGEX.search(line)
    if match:
        return [
            int(match.group(1)), int(match.group(2)), int(match.group(3)), # Accel
            int(match.group(4)), int(match.group(5)), int(match.group(6)), # Gyro
            int(match.group(7)), int(match.group(8)), int(match.group(9))  # Mag
        ]
    return None

def read_client_output(proc, buffer):
    while True:
        line = proc.stdout.readline()
        if not line:
            break
        line = line.decode(errors='ignore').strip()
        if line:
            buffer.append(line)
            if len(buffer) > 100:
                buffer.pop(0)

def get_orientation(x, y, z):
    # Convert milli-g to g for trig
    x /= 1000.0
    y /= 1000.0
    z /= 1000.0
    pitch = degrees(atan2(-x, sqrt(y * y + z * z)))
    roll  = degrees(atan2(y, z))
    return pitch, roll

def draw_3d_board(ax3d, pitch, roll):
    ax3d.clear()
    size = 1.0

    board = np.array([
        [-size, -size, 0],
        [ size, -size, 0],
        [ size,  size, 0],
        [-size,  size, 0]
    ])

    pitch_rad = np.radians(pitch)
    roll_rad  = np.radians(roll)

    R_pitch = np.array([
        [1,                0,                 0],
        [0,  np.cos(pitch_rad), -np.sin(pitch_rad)],
        [0,  np.sin(pitch_rad),  np.cos(pitch_rad)]
    ])

    R_roll = np.array([
        [ np.cos(roll_rad), 0, np.sin(roll_rad)],
        [0,                 1,               0],
        [-np.sin(roll_rad), 0, np.cos(roll_rad)]
    ])

    R = R_roll @ R_pitch
    rotated_board = board @ R.T

    verts = [rotated_board]
    ax3d.add_collection3d(
        Poly3DCollection(verts, color='lightblue', alpha=0.8, edgecolor='k')
    )

    # Red line perpendicular to orange (board-relative Y direction)
    red_start = R @ np.array([0.5, -0.3, 0.0])
    red_end   = R @ np.array([0.5,  0.3, 0.0])
    ax3d.plot(
        [red_start[0], red_end[0]],
        [red_start[1], red_end[1]],
        [red_start[2], red_end[2]],
        color='red', linewidth=3
    )

    # World-frame coordinate arrows
    origin = np.array([0, 0, 0])
    ax3d.quiver(*origin, 0, 1, 0, color='r', length=1.5, arrow_length_ratio=0.1)
    ax3d.quiver(*origin, 1, 0, 0, color='g', length=1.5, arrow_length_ratio=0.1)
    ax3d.quiver(*origin, 0, 0, -1, color='b', length=1.5, arrow_length_ratio=0.1)

    ax3d.text(0,   1.6,  0,    'X', color='r', fontsize=10)
    ax3d.text(1.6, 0,    0,    'Y', color='g', fontsize=10)
    ax3d.text(0,   0,   -1.6,  'Z', color='b', fontsize=10)

    ax3d.set_xlim(-2, 2)
    ax3d.set_ylim(-2, 2)
    ax3d.set_zlim(-2, 2)
    ax3d.set_title("3D Board Orientation (RTSP SEI Data)")

def animate(i, data, axs, ax3d, buffer):
    for line in reversed(buffer[-10:]):
        vals = parse_line(line)
        if vals:
            # Unpack
            ax, ay, az, gx, gy, gz, mx, my, mz = vals
            
            # Append to history
            data['ax'].append(ax); data['ay'].append(ay); data['az'].append(az)
            data['gx'].append(gx); data['gy'].append(gy); data['gz'].append(gz)
            data['mx'].append(mx); data['my'].append(my); data['mz'].append(mz)

            # Keep window size
            for k in data:
                if len(data[k]) > 50:
                    data[k].pop(0)

            # Draw Accel
            axs[0].clear()
            axs[0].plot(data['ax'], label='X (mg)', color='r')
            axs[0].plot(data['ay'], label='Y (mg)', color='g')
            axs[0].plot(data['az'], label='Z (mg)', color='b')
            axs[0].set_ylim(-2000, 2000)
            axs[0].legend(loc='upper right')
            axs[0].set_title("Accelerometer")

            # Draw Gyro
            axs[1].clear()
            axs[1].plot(data['gx'], label='X (mdps)', color='r')
            axs[1].plot(data['gy'], label='Y (mdps)', color='g')
            axs[1].plot(data['gz'], label='Z (mdps)', color='b')
            axs[1].set_ylim(-2000, 2000)
            axs[1].legend(loc='upper right')
            axs[1].set_title("Gyroscope")

            # Draw Mag
            axs[2].clear()
            axs[2].plot(data['mx'], label='X (uT)', color='r')
            axs[2].plot(data['my'], label='Y (uT)', color='g')
            axs[2].plot(data['mz'], label='Z (uT)', color='b')
            axs[2].set_ylim(-100, 100)
            axs[2].legend(loc='upper right')
            axs[2].set_title("Magnetometer")

            # Draw 3D Model using Accel
            pitch, roll = get_orientation(ax, ay, az)
            draw_3d_board(ax3d, pitch, roll)

            break

def main():
    if len(sys.argv) > 1:
        ip = sys.argv[1]
    else:
        ip = "192.168.1.100"

    print(f"[INFO] Starting SEI Client reading from IP: {ip}...")
    
    # Spawn sei_client and capture its output live
    proc = subprocess.Popen(
        ["./sei_client", "--ip", ip],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    data = {
        'ax': [], 'ay': [], 'az': [],
        'gx': [], 'gy': [], 'gz': [],
        'mx': [], 'my': [], 'mz': []
    }
    buffer = []

    fig = plt.figure(figsize=(14, 8))
    
    # Create subplots
    ax_accel = fig.add_subplot(321)
    ax_gyro  = fig.add_subplot(323)
    ax_mag   = fig.add_subplot(325)
    ax3d     = fig.add_subplot(122, projection='3d')

    axs = [ax_accel, ax_gyro, ax_mag]
    plt.tight_layout()

    ani = animation.FuncAnimation(
        fig, animate,
        fargs=(data, axs, ax3d, buffer),
        interval=50,
        cache_frame_data=False
    )

    thread = threading.Thread(target=read_client_output, args=(proc, buffer), daemon=True)
    thread.start()

    plt.show()

    print("[INFO] Shutting down SEI Client...")
    proc.terminate()

if __name__ == "__main__":
    main()
