/*
 * rtsp_server.h — Lazy RTSP Factory
 * ===================================
 * Port of: unified_engine.py — LazyRTSPFactory class (lines 570–730)
 *
 * Encoder branch created on capture tee ONLY when first client connects
 * (media-configure). Torn down when last client disconnects (unprepared).
 */

#ifndef SMARTIP_RTSP_SERVER_H
#define SMARTIP_RTSP_SERVER_H

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include "subsystems/sys_capture.h"
#include "subsystems/sys_stream.h"

/* Forward declaration for engine reference */
typedef struct _UnifiedEngine UnifiedEngine;

/*
 * lazy_rtsp_factory_create — Create and configure a LazyRTSPFactory.
 *
 * @param engine     Pointer to the owning UnifiedEngine
 * @param cap        Capture pipeline to attach encoder branches to
 * @param lens       Lens identifier (e.g. "lens1")
 * @param tier       Stream tier (e.g. "main", "sub")
 * @param cfg        Branch config (resolution, fps, codec)
 * @param mount_path RTSP mount path (e.g. "/lens1/main")
 */
GstRTSPMediaFactory *lazy_rtsp_factory_create(UnifiedEngine *engine,
                                                CapturePipeline *cap,
                                                StreamManager *stream_mgr,
                                                const char *lens,
                                                const char *tier,
                                                BranchConfig *branch_cfg,
                                                const char *mount_path,
                                                gboolean cairo_enabled);

/* Start the RTSP server on the given port */
GstRTSPServer *rtsp_server_start(int port);

void          lazy_rtsp_factory_dump_dot(GstRTSPMediaFactory *factory, const char *ts);

#endif /* SMARTIP_RTSP_SERVER_H */
