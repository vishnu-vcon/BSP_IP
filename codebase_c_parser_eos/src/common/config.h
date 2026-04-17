/*
 * config.h — Shared Constants and D-Bus Interface Definitions
 * ============================================================
 * Port of: common/dbus_interfaces.py + common/event_broker.py (constants)
 *
 * Defines IPC routing mappings for all processes:
 *   - D-Bus bus names and object paths
 *   - ZeroMQ topics
 *   - D-Bus introspection XML
 */

#ifndef SMARTIP_CONFIG_H
#define SMARTIP_CONFIG_H

/* ── D-Bus: Unified Engine (camera-unified.service) ── */
#define UNIFIED_BUS_NAME "com.camera.UnifiedEngine"
#define UNIFIED_BUS_PATH "/com/camera/UnifiedEngine"

/* ── ZeroMQ Topics ── */
#define TOPIC_AI_COORDS  "events/ai_coordinates"
#define TOPIC_ALERTS     "events/alerts"
#define ZMQ_DEFAULT_PORT 5555

/* ── HTTP/HTTPS Defaults ── */
#define DEFAULT_HTTP_PORT   8443
#define DEFAULT_RTSP_PORT   8554
#define DEFAULT_MJPEG_PORT  8080
#define DEFAULT_DATA_DIR    "/data"

/* ── Default Config File ── */
#define DEFAULT_CONFIG_PATH "config/default_config.json"

/* ── D-Bus Introspection XML ── */
static const gchar UNIFIED_IFACE_XML[] =
    "<node>"
    "  <interface name='com.camera.UnifiedEngine'>"
    "    <!-- Video Methods -->"
    "    <method name='ConfigureLens'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='UpdateStreamParams'>"
    "      <arg direction='in'  name='lens_id'     type='s'/>"
    "      <arg direction='in'  name='branch'      type='s'/>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='StopLens'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='TakeSnapshot'>"
    "      <arg direction='in'  name='lens_id'     type='s'/>"
    "      <arg direction='in'  name='output_path' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='StartRecording'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='StopRecording'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='StartHLS'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='StopHLS'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='GetStatus'>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='SetLogLevel'>"
    "      <arg direction='in'  name='level'       type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <!-- AI Methods -->"
    "    <method name='AttachSHM'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='DetachSHM'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='ToggleNTP'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='ToggleOverlay'>"
    "      <arg direction='in'  name='params_json' type='s'/>"
    "      <arg direction='out' name='response'    type='s'/>"
    "    </method>"
    "    <method name='SaveCurrentConfig'>"
    "      <arg direction='out' name='response'    type='b'/>"
    "    </method>"
    "  </interface>"
    "</node>";

#endif /* SMARTIP_CONFIG_H */
