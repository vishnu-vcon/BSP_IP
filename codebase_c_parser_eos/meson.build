# ══════════════════════════════════════════════════════════════
# SMART IP Edge Camera Application — Meson Build
# ══════════════════════════════════════════════════════════════
# Produces 4 binaries:
#   1. smartip_engine       — Unified Camera Engine (GStreamer + D-Bus server)
#   2. smartip_control      — Control Plane (HTTP + D-Bus client)
#   3. smartip_alerts       — Alert Manager (ZMQ subscriber)
#   4. smartip_cli          — Interactive CLI (D-Bus client)
#
# Build:   meson setup build && ninja -C build
# Install: ninja -C build install
# ══════════════════════════════════════════════════════════════

project('smartip-edge', 'c',
  version : '2.0.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
  ]
)

# ── Version Configurations ──
# Explicitly support both Compulab (1.24.7) and NXP EVK (1.26.0)
gst_req_version = '>=1.24.0'

# ── Dependencies ──
gst_dep       = dependency('gstreamer-1.0',              version : gst_req_version, required : true)
gst_app_dep   = dependency('gstreamer-app-1.0',          version : gst_req_version, required : true)
gst_rtsp_dep  = dependency('gstreamer-rtsp-server-1.0',  version : gst_req_version, required : true)
gio_dep       = dependency('gio-2.0',                    required : true)
glib_dep      = dependency('glib-2.0',                   required : true)
json_dep      = dependency('json-glib-1.0',              required : true)
soup_dep      = dependency('libsoup-2.4',                required : false)
zmq_dep       = dependency('libzmq',                     required : false)
cairo_dep     = dependency('cairo',                      required : true)
sqlite_dep    = dependency('sqlite3',                    required : true)
crypt_dep     = meson.get_compiler('c').find_library('crypt', required : true)
ssl_dep       = dependency('openssl',                    required : true)

# Optional: Only needed by smartip_control (OTP QR code generation)
# If missing, smartip_control will not be built.
qrencode_dep  = dependency('libqrencode',                required : false)
png_dep       = dependency('libpng',                     required : false)
curl_dep      = dependency('libcurl',                    required : false)

# Fallback: find zmq manually if pkg-config doesn't work
if not zmq_dep.found()
  zmq_dep = meson.get_compiler('c').find_library('zmq', required : true)
endif

# ── Common sources (linked into all binaries) ──
common_src = files(
  'src/common/event_broker.c',
)
common_inc = include_directories('src/common', 'src')

# ══════════════════════════════════════════════════════════════
# Binary 1: smartip_engine (Unified Camera Engine)
# ══════════════════════════════════════════════════════════════
engine_src = files(
  'src/engine/main.c',
  'src/engine/subsystems/sys_capture.c',
  'src/engine/gst_blocks/encoder_builder.c',
  'src/engine/subsystems/sys_rtsp.c',
  'src/engine/subsystems/sys_stream.c',
  'src/engine/subsystems/sys_hls.c',
  'src/engine/subsystems/sys_recording.c',
  'src/engine/subsystems/sys_config.c',
  'src/engine/api/dbus_handlers.c',
)

executable('smartip_engine',
  engine_src + common_src,
  dependencies : [gst_dep, gst_app_dep, gst_rtsp_dep, gio_dep, glib_dep, json_dep, zmq_dep, cairo_dep],
  include_directories : [common_inc],
  install : true,
)

# ══════════════════════════════════════════════════════════════
# Binary 2: smartip_control (Control Plane)
# ══════════════════════════════════════════════════════════════
control_src = files(
  'src/control_plane/main.c',
  'src/control_plane/core/auth.c',
  'src/control_plane/core/http_server.c',
  'src/control_plane/auth/db_manager.c',
  'src/control_plane/auth/password_hash.c',
  'src/control_plane/auth/password_policy.c',
  'src/control_plane/auth/otp_manager.c',
  'src/control_plane/auth/smtp_sender.c',
  'src/control_plane/auth/activity_logger.c',
  'src/control_plane/auth/role_manager.c',
  'src/control_plane/auth/session_manager.c',
)

control_all_deps_found = soup_dep.found() and qrencode_dep.found() and png_dep.found() and curl_dep.found()

if control_all_deps_found
  executable('smartip_control',
    control_src + common_src,
    dependencies : [gio_dep, glib_dep, json_dep, soup_dep, zmq_dep,
                    sqlite_dep, crypt_dep, ssl_dep, qrencode_dep, png_dep, curl_dep],
    include_directories : [common_inc],
    install : true,
  )
else
  _missing = []
  if not soup_dep.found()
    _missing += 'libsoup-2.4'
  endif
  if not qrencode_dep.found()
    _missing += 'libqrencode'
  endif
  if not png_dep.found()
    _missing += 'libpng'
  endif
  if not curl_dep.found()
    _missing += 'libcurl'
  endif
  warning('smartip_control will NOT be built — missing: ' + ', '.join(_missing))
endif

# ══════════════════════════════════════════════════════════════
# Binary 3: smartip_alerts (Alert Manager)
# ══════════════════════════════════════════════════════════════
executable('smartip_alerts',
  'src/alert_manager/main.c',
  common_src,
  dependencies : [glib_dep, json_dep, zmq_dep],
  include_directories : [common_inc],
  install : true,
)

# ══════════════════════════════════════════════════════════════
# Binary 4: smartip_cli (Web Client Mock in C)
# ══════════════════════════════════════════════════════════════
if soup_dep.found()
  executable('smartip_cli',
    'src/cli/main.c',
    dependencies : [gio_dep, glib_dep, json_dep, soup_dep],
    include_directories : [common_inc],
    install : true,
  )

  # ══════════════════════════════════════════════════════════════
  # Binary 5: smartip_hls_test (Standalone HLS Test Server + Player)
  # ══════════════════════════════════════════════════════════════
  executable('smartip_hls_test',
    'src/tools/hls_test_server.c',
    dependencies : [glib_dep, soup_dep],
    install : true,
  )
endif

# ══════════════════════════════════════════════════════════════
# Python Components: AI Engine + CLI (install as data, not compiled)
# ══════════════════════════════════════════════════════════════
ai_install_dir = get_option('datadir') / 'smartip' / 'ai_engine'

# Core AI engine scripts
install_data(
  'src/ai_engine/ai_engine.py',
  'src/ai_engine/ai_utils_person.py',
  'src/ai_engine/ai_utils_helmet.py',
  'src/ai_engine/ai_utils_overspeed.py',
  'src/ai_engine/ai_utils_triding.py',
  'src/ai_engine/new_ai_utils_person.py',
  install_dir : ai_install_dir,
)

# Common modules (event_broker for ZMQ pub/sub)
install_data(
  'src/ai_engine/common/event_broker.py',
  install_dir : ai_install_dir / 'common',
)

# AI model scripts + TFLite weights
install_data(
  'src/ai_engine/ai_models/Helmet_Detection/helmet.py',
  'src/ai_engine/ai_models/Helmet_Detection/saved_model_converted_quantized_320.tflite',
  install_dir : ai_install_dir / 'ai_models' / 'Helmet_Detection',
)
install_data(
  'src/ai_engine/ai_models/Triple_Riding/triding.py',
  'src/ai_engine/ai_models/Triple_Riding/saved_model_triding_320.tflite',
  install_dir : ai_install_dir / 'ai_models' / 'Triple_Riding',
)
install_data(
  'src/ai_engine/ai_models/overspeed/overspeed_rtsp.py',
  'src/ai_engine/ai_models/overspeed/model_calibrated_int8_rog.tflite',
  install_dir : ai_install_dir / 'ai_models' / 'overspeed',
)

# Python CLI (executable script)
install_data(
  'src/cli/smartip_cli.py',
  install_dir : get_option('bindir'),
  install_mode : 'rwxr-xr-x',
)

# ══════════════════════════════════════════════════════════════
# Automated Verification (Unit Tests)
# ══════════════════════════════════════════════════════════════
cmocka_dep = dependency('cmocka', required : false)

if cmocka_dep.found()
  # Test 1: Verify Auth Flow (Mocking HMAC)
  test_auth_src = files('tests/verify_auth_flow.c', 'src/control_plane/core/auth.c')
  executable('verify_auth_flow',
    test_auth_src,
    dependencies : [glib_dep, json_dep, cmocka_dep],
    include_directories : [common_inc, include_directories('src/control_plane')],
    c_args : ['-DUNIT_TESTING'],
    link_args : [
      '-Wl,--wrap=g_hmac_new',
      '-Wl,--wrap=g_hmac_update',
      '-Wl,--wrap=g_hmac_get_string',
      '-Wl,--wrap=g_hmac_unref'
    ]
  )
  test('verify_auth_flow', find_program('verify_auth_flow'))

  # Test 2: Verify Pipeline Logic (Mocking GStreamer)
  test_pipeline_src = files('tests/verify_pipeline_logic.c', 'src/engine/gst_blocks/encoder_builder.c')
  executable('verify_pipeline_logic',
    test_pipeline_src + common_src,
    dependencies : [gst_dep, glib_dep, cmocka_dep],
    include_directories : [common_inc, include_directories('src/engine')],
    c_args : ['-DUNIT_TESTING'],
    link_args : [
      '-Wl,--wrap=gst_element_factory_make'
    ]
  )
  test('verify_pipeline_logic', find_program('verify_pipeline_logic'))
endif
