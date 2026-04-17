/*
 * stream_manager.h — Shared Encoder Management
 * ============================================
 * Manages the lifecycle of encoder branches on the capture pipeline.
 * Allows multiple protocols (RTSP, HLS) to share a single hardware encoder instance.
 */

#ifndef SMARTIP_STREAM_MANAGER_H
#define SMARTIP_STREAM_MANAGER_H

#include <glib.h>
#include <gst/gst.h>
#include "subsystems/sys_capture.h"

typedef struct _StreamManager StreamManager;

/* Callback signature for encoded buffers */
typedef void (*StreamBufferFunc)(GstBuffer *buffer, gpointer user_data);

StreamManager *stream_manager_new(void);
void           stream_manager_free(StreamManager *manager);

/* 
 * Acquire an encoder for a specific lens/tier combination.
 * If the encoder branch doesn't exist, it will be created on the provided capture pipeline.
 */
gboolean stream_manager_acquire(StreamManager *manager, 
                               CapturePipeline *capture_pipe,
                               const char *lens, 
                               const char *tier, 
                               BranchConfig *branch_config,
                               gboolean cairo_enabled);

/* 
 * Release an encoder. If refcount hits 0, the branch is removed.
 */
void stream_manager_release(StreamManager *manager, 
                            const char *lens, 
                            const char *tier);

/* 
 * Register a callback to receive encoded buffers for a specific lens/tier.
 * Returns a handler ID that can be used to unregister.
 */
gulong stream_manager_subscribe(StreamManager *manager,
                               const char *lens,
                               const char *tier,
                               StreamBufferFunc func,
                               gpointer user_data);

void stream_manager_unsubscribe(StreamManager *manager,
                               const char *lens,
                               const char *tier,
                               gulong handler_id);

/* 
 * Returns the exact GStreamer branch name (e.g. "shared_lens1_main_123")
 * for a lens/tier if it exists and is active. Returns NULL if idle.
 * The returned string belongs to the manager and must NOT be freed.
 */
const char *stream_manager_get_active_branch_name(StreamManager *manager,
                                                  const char *lens,
                                                  const char *tier);

/* Force teardown of a specific stream tier (e.g. for codec reconfiguration) */
void           stream_manager_force_teardown(StreamManager *manager, const char *lens, const char *tier);

#endif /* SMARTIP_STREAM_MANAGER_H */
