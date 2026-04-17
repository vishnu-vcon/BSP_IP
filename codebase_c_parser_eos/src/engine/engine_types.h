#ifndef SMARTIP_ENGINE_TYPES_H
#define SMARTIP_ENGINE_TYPES_H

#include <glib.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <json-glib/json-glib.h>
#include "../common/event_broker.h"
#include "subsystems/sys_capture.h"
#include "subsystems/sys_stream.h"

/* Forward declarations to prevent circular dependencies */
typedef struct _RecordingBranch RecordingBranch;
typedef struct _HLSGenerator HLSGenerator;

/* ── Lens Info (port of self._lenses dict) ── */
typedef struct {
  char device[64];
  gboolean cairo_capable;
  gboolean overlay_enabled;
  double ai_threshold;
  double snapshot_threshold;
  BranchConfig *rec_defaults;
} LensInfo;

/* ── Unified Engine (The Root Controller) ── */
typedef struct _UnifiedEngine {
  GMainLoop *loop;
  GstRTSPServer *rtsp_server;
  MQSubscriber *mq_sub;
  guint dbus_owner_id;
  GDBusProxy *ai_proxy;

  /* Lens management (port of self._lenses) */
  GHashTable *lenses;         /* "lens1" → LensInfo* */
  GHashTable *captures;       /* device → CapturePipeline* */
  GHashTable *branch_configs; /* "lens1/main" → BranchConfig* */
  GHashTable *recordings;     /* "lens/tier" → RecordingBranch* */

  /* RTSP client tracking (port of _active_branches, _session_mounts) */
  GHashTable *active_branches; /* mount → client_count */
  GHashTable *session_mounts;  /* session_id → mount */
  int total_clients;

  /* Shared Stream Management */
  StreamManager *stream_mgr;
  GHashTable *hls_generators; /* "lens/tier" → HLSGenerator* */

  /* Robust Scheduler */
  GThread *scheduler_thread;
  GCond scheduler_cond;
  GMutex scheduler_mutex;
  GMutex config_mutex;
  gboolean scheduler_running;

  /* Config */
  int rtsp_port;
  char config_path[256];
  char default_caps[256];
} UnifiedEngine;

/* ── Engine Logic Exports (implemented in main.c) ── */
char *engine_configure_lens(UnifiedEngine *e, const char *json);

#endif /* SMARTIP_ENGINE_TYPES_H */
