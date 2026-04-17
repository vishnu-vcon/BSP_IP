/*
 * sys_rtsp.c — Lazy RTSP Factory  [INSTRUMENTED]
 * =================================================
 *
 * BUG FIXES:
 *   [FIX-STALE-CFG] _on_media_configure previously used factory-creation-time
 *     values (rtsp->ntp_overlay, rtsp->cairo_enabled) when building the
 *     BranchConfig passed to stream_manager_acquire().  These values were
 *     populated once at lazy_rtsp_factory_create() and never updated, so any
 *     change made via engine_toggle_ntp() / engine_update_stream() /
 *     engine_configure_lens() to BranchConfig or LensInfo was silently
 *     discarded the moment a new client connected.  Fix: look up the live
 *     BranchConfig and LensInfo from the engine at connect time and use those.
 *
 *   [FIX-MUTEX] Serialized stream_manager_acquire() with engine->config_mutex
 *     to prevent GStreamer type-registry races on concurrent pipeline mutations.
 *
 * CPU-COPY FIX (2025-04):
 *   [OPT-COPY] _on_encoded_buffer: buf delivered by sys_stream.c as a
 *   pre-made gst_buffer_copy_region (FLAGS|TIMESTAMPS|META) copy.  Refcount
 *   is 1 at entry — adjust PTS/DTS in-place, push to appsrc.  No extra copy.
 *
 * NAMING IMPROVEMENTS:
 *   - LazyRTSPData pointer renamed from `d` to `rtsp` everywhere.
 *   - `tmp_cfg`       → branch_cfg
 *   - `element`       → media_pipeline_element
 *   - `ok`            → stream_acquired
 *   - `ret`           → push_result
 *   - `mem` / `mem_type` → memory_block / memory_type
 *   - `ttff`          → time_to_first_frame_ms
 *   - `pts` / `dts`   → orig_pts / orig_dts
 */

#include "subsystems/sys_rtsp.h"
#include "engine_types.h"
#include "gst_blocks/encoder_builder.h"
#include "subsystems/sys_stream.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    UnifiedEngine   *engine;
    CapturePipeline *capture;
    StreamManager   *stream_mgr;
    char             lens[32];
    char             tier[32];
    char             codec[16];
    int              width, height, fps, bitrate;
    gboolean         cairo_enabled;
    gboolean         ntp_overlay;
    char             mount_path[64];
    char             branch_name[64];

    GstElement      *appsrc;
    guint64          base_pts;
    gboolean         base_pts_set;
    gulong           handler_id;
    int              frame_count;
    GstRTSPMedia    *active_media;
    gint64           start_time;

    /* Instrumentation */
    volatile gint    frames_pushed;
    volatile gint    frames_failed;

    GMutex           branch_lock;
    volatile gint    refcount;
} LazyRTSPData;

/* ── Reference Counting Helpers ── */
static void lazy_rtsp_data_ref(LazyRTSPData *rtsp) {
    if (!rtsp) return;
    g_atomic_int_inc(&rtsp->refcount);
}

static void lazy_rtsp_data_unref(gpointer data) {
    LazyRTSPData *rtsp = (LazyRTSPData *)data;
    if (!rtsp) return;
    if (g_atomic_int_dec_and_test(&rtsp->refcount)) {
        g_debug("[RTSP] Finalizing LazyRTSPData for '%s' (refcount reached 0)",
                rtsp->mount_path);
        g_mutex_clear(&rtsp->branch_lock);
        g_free(rtsp);
    }
}

static void _lazy_rtsp_data_signal_notify(gpointer data, GClosure *closure) {
    (void)closure;
    lazy_rtsp_data_unref(data);
}

static GQuark _lazy_rtsp_quark(void) {
    return g_quark_from_static_string("lazy-rtsp-data");
}

/* ── StreamManager Callback → bridge to appsrc ── */
/*
 * `buf` is delivered by sys_stream.c _on_new_sample as an already-copied
 * GstBuffer header (gst_buffer_copy_region FLAGS|TIMESTAMPS|META).
 * Refcount == 1 at entry — adjust PTS/DTS in-place and push to appsrc.
 */
static void _on_encoded_buffer(GstBuffer *buf, gpointer user_data) {
    LazyRTSPData *rtsp = (LazyRTSPData *)user_data;

    g_mutex_lock(&(rtsp->branch_lock));

    if (!rtsp->appsrc) {
        g_mutex_unlock(&rtsp->branch_lock);
        /* buf was allocated for us; release it */
        gst_buffer_unref(buf);
        return;
    }

    /* Prove Zero-Copy Path on first few frames */
    if (rtsp->frame_count < 5) {
        GstMemory  *memory_block = gst_buffer_peek_memory(buf, 0);
        const char *memory_type  = (memory_block && memory_block->allocator &&
                                    memory_block->allocator->mem_type)
                                   ? memory_block->allocator->mem_type
                                   : "unknown";
        gboolean    is_hw        = (g_strcmp0(memory_type, "dmabuf") == 0);
        double      time_to_first_frame_ms =
            (double)(g_get_monotonic_time() - rtsp->start_time) / 1000.0;

        g_info("[DEBUG-MEM] Lens %s (%s): Frame %d arrived!  Size: %u  "
               "Memory: %s  TTFF: %.2f ms",
               rtsp->lens, rtsp->tier, rtsp->frame_count + 1,
               (guint)gst_buffer_get_size(buf),
               is_hw ? "HARDWARE (Optimized)" : "SYSTEM (Stable)",
               time_to_first_frame_ms);
    }

    guint64 orig_pts = GST_BUFFER_PTS(buf);
    guint64 orig_dts = GST_BUFFER_DTS(buf);

    /* Safely establish base_pts ignoring NONE values */
    if (!rtsp->base_pts_set && orig_pts != GST_CLOCK_TIME_NONE) {
        rtsp->base_pts     = orig_pts;
        rtsp->base_pts_set = TRUE;
        g_debug("[RTSP-BR] '%s'  base_pts established: %" G_GUINT64_FORMAT " ms",
                rtsp->mount_path, rtsp->base_pts / GST_MSECOND);
    }

    /* Safe rebasing: prevent guint64 underflow on jittered or NONE timestamps */
    if (orig_pts != GST_CLOCK_TIME_NONE) {
        GST_BUFFER_PTS(buf) = (orig_pts >= rtsp->base_pts)
                              ? (orig_pts - rtsp->base_pts) : 0;
    } else {
        GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
    }

    if (orig_dts != GST_CLOCK_TIME_NONE) {
        GST_BUFFER_DTS(buf) = (orig_dts >= rtsp->base_pts)
                              ? (orig_dts - rtsp->base_pts) : 0;
    } else {
        GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;
    }

    /* gst_app_src_push_buffer takes ownership of buf — do NOT unref after this */
    GstFlowReturn push_result = gst_app_src_push_buffer(GST_APP_SRC(rtsp->appsrc), buf);
    if (push_result != GST_FLOW_OK) {
        g_atomic_int_inc(&rtsp->frames_failed);
        g_debug("[RTSP-BR] '%s'  push_buffer FAILED: %s  total_fails=%d",
                rtsp->mount_path, gst_flow_get_name(push_result),
                g_atomic_int_get(&rtsp->frames_failed));
    } else {
        g_atomic_int_inc(&rtsp->frames_pushed);
    }
    rtsp->frame_count++;
    g_mutex_unlock(&rtsp->branch_lock);
}

/* ── media-configure: first client connects ── */
static void _on_media_configure(GstRTSPMediaFactory *factory,
                                GstRTSPMedia *media, gpointer user_data) {
    (void)user_data;

    LazyRTSPData *rtsp = g_object_get_qdata(G_OBJECT(factory), _lazy_rtsp_quark());
    if (!rtsp) {
        g_warning("[TRACE]   NULL LazyRTSPData for factory %p", factory);
        return;
    }
    if (!rtsp->capture || !rtsp->stream_mgr) {
        g_warning("[TRACE]   Missing capture (%p) or mgr (%p) for %s",
                  rtsp->capture, rtsp->stream_mgr, rtsp->mount_path);
        return;
    }

    g_mutex_lock(&(rtsp->branch_lock));

    g_info("[RTSP] Client connected to '%s'  codec=%s  %dx%d @ %d fps",
           rtsp->mount_path, rtsp->codec, rtsp->width, rtsp->height, rtsp->fps);

    /* ── BUG FIX [FIX-STALE-CFG] ──────────────────────────────────────────────
     * Build branch_cfg from the LIVE BranchConfig and LensInfo rather than the
     * factory's stale snapshot.  The factory's rtsp->ntp_overlay and
     * rtsp->cairo_enabled are set once at factory-creation time and are never
     * updated when the user calls ToggleNTP, UpdateStreamParams, or
     * ConfigureLens.  Reading from engine->branch_configs and engine->lenses
     * here guarantees we always stream with the latest user-requested settings,
     * even after a reconnect without a full pipeline rebuild.
     * ─────────────────────────────────────────────────────────────────────── */
    BranchConfig branch_cfg = {
        .width = rtsp->width, .height = rtsp->height, .fps = rtsp->fps, .bitrate = rtsp->bitrate};
    g_strlcpy(branch_cfg.codec, rtsp->codec, sizeof(branch_cfg.codec));

    /* Defaults from stale snapshot in case live lookup fails */
    branch_cfg.ntp_overlay = rtsp->ntp_overlay;
    gboolean effective_cairo = rtsp->cairo_enabled;

    char cfg_lookup_key[128];
    snprintf(cfg_lookup_key, sizeof(cfg_lookup_key), "%s/%s", rtsp->lens, rtsp->tier);

    BranchConfig *live_branch_cfg =
        g_hash_table_lookup(rtsp->engine->branch_configs, cfg_lookup_key);
    if (live_branch_cfg) {
        /* Use the live ntp_overlay — reflects latest ToggleNTP / UpdateStreamParams */
        branch_cfg.ntp_overlay = live_branch_cfg->ntp_overlay;

        /* Recompute effective cairo from live LensInfo (cairo_capable) combined
         * with the live per-branch overlay_enabled flag. */
        LensInfo *live_lens_info = g_hash_table_lookup(rtsp->engine->lenses, rtsp->lens);
        if (live_lens_info) {
            gboolean lens_supports_cairo = live_lens_info->cairo_capable;
            gboolean branch_overlay_on   = live_branch_cfg->overlay_enabled;
            gboolean is_third_tier       = (g_strcmp0(rtsp->tier, "third") == 0);

            effective_cairo = lens_supports_cairo && branch_overlay_on && !is_third_tier;
        }

        /* Keep the factory's local copies in sync for future logging */
        rtsp->ntp_overlay    = branch_cfg.ntp_overlay;
        rtsp->cairo_enabled  = effective_cairo;

        g_info("[RTSP] '%s' using LIVE config: ntp_overlay=%s  cairo=%s",
               rtsp->mount_path,
               branch_cfg.ntp_overlay ? "TRUE" : "FALSE",
               effective_cairo        ? "TRUE" : "FALSE");
    } else {
        g_warning("[RTSP] '%s' live BranchConfig not found — using factory snapshot",
                  rtsp->mount_path);
    }

    g_info("[TRACE]   Calling stream_manager_acquire for %s/%s...", rtsp->lens, rtsp->tier);

    /* [FIX-MUTEX] Serialize acquisition with config_mutex to prevent
     * GStreamer type registry races and concurrent pipeline mutations. */
    g_mutex_lock(&rtsp->engine->config_mutex);
    gboolean stream_acquired = stream_manager_acquire(rtsp->stream_mgr, rtsp->capture,
                                                      rtsp->lens, rtsp->tier,
                                                      &branch_cfg, effective_cairo);
    g_mutex_unlock(&rtsp->engine->config_mutex);

    g_info("[TRACE]   stream_manager_acquire returned %s",
           stream_acquired ? "TRUE" : "FALSE");

    if (!stream_acquired) {
        g_warning("[RTSP] FAILED to acquire encoder for '%s'", rtsp->mount_path);
        g_mutex_unlock(&rtsp->branch_lock);
        return;
    }

    GstElement *media_pipeline_element = gst_rtsp_media_get_element(media);
    if (media_pipeline_element) {
        rtsp->appsrc = gst_bin_get_by_name(GST_BIN(media_pipeline_element), "pay_src");
        gst_object_unref(media_pipeline_element);
    }

    if (!rtsp->appsrc) {
        g_warning("[RTSP] Could not find 'pay_src' appsrc element for '%s'",
                  rtsp->mount_path);
    }

    rtsp->base_pts_set = FALSE;
    rtsp->frame_count  = 0;
    rtsp->frames_pushed = 0;
    rtsp->frames_failed = 0;
    rtsp->start_time   = g_get_monotonic_time();

    rtsp->handler_id = stream_manager_subscribe(rtsp->stream_mgr, rtsp->lens, rtsp->tier,
                                                _on_encoded_buffer, rtsp);

    rtsp->active_media = media;
    g_mutex_unlock(&rtsp->branch_lock);
    g_info("[TRACE] _on_media_configure FINISHED for %s", rtsp->mount_path);
    g_info("[RTSP] ✓ Encoder bridge active for '%s'", rtsp->mount_path);
}

/* ── unprepared: last client disconnects ── */
static void _on_unprepared(GstRTSPMedia *media, gpointer user_data) {
    (void)media;
    LazyRTSPData *rtsp = (LazyRTSPData *)user_data;

    g_info("[RTSP] All clients disconnected from '%s'  frames_pushed=%d  push_fails=%d",
           rtsp->mount_path, g_atomic_int_get(&rtsp->frames_pushed),
           g_atomic_int_get(&rtsp->frames_failed));

    /* Unsubscribe OUTSIDE lock first — guarantees no more _on_encoded_buffer
     * calls will arrive after this returns. */
    if (rtsp->stream_mgr) {
        g_debug("[RTSP] Unsubscribing handler_id=%lu from StreamManager",
                rtsp->handler_id);
        stream_manager_unsubscribe(rtsp->stream_mgr, rtsp->lens, rtsp->tier,
                                   rtsp->handler_id);
    }

    /* Null appsrc under lock, then release lock BEFORE calling
     * stream_manager_release.
     *
     * WHY: stream_manager_release → capture_pipeline_remove_branch acquires
     * cap->branch_mutex. If we hold branch_lock here while another thread
     * holds branch_mutex and waits for branch_lock, that is a classic AB/BA
     * deadlock. Releasing branch_lock before the release() call breaks it. */
    g_mutex_lock(&(rtsp->branch_lock));
    g_clear_object(&rtsp->appsrc);
    rtsp->active_media  = NULL;
    rtsp->base_pts_set  = FALSE;
    rtsp->frame_count   = 0;
    g_mutex_unlock(&rtsp->branch_lock);

    /* Now safe to call release — no branch_lock held */
    if (rtsp->stream_mgr) {
        stream_manager_release(rtsp->stream_mgr, rtsp->lens, rtsp->tier);
    }
}

/* Media-constructed callback: connects the unprepared signal */
static void _on_media_constructed(GstRTSPMediaFactory *factory,
                                  GstRTSPMedia *media, gpointer user_data) {
    (void)factory;
    LazyRTSPData *rtsp = (LazyRTSPData *)user_data;

    lazy_rtsp_data_ref(rtsp);
    g_info("[RTSP] Media constructed for '%s' — Handshake in progress... (refs=%d)",
           rtsp->mount_path, g_atomic_int_get(&rtsp->refcount));

    g_signal_connect_data(media, "unprepared", G_CALLBACK(_on_unprepared), rtsp,
                          (GClosureNotify)_lazy_rtsp_data_signal_notify, 0);

    g_debug("[RTSP] 'unprepared' signal connected for media on '%s' (refcount=%d)",
            rtsp->mount_path, g_atomic_int_get(&rtsp->refcount));
}

/* ══════════════════════════════════════════════════════════════ */

GstRTSPMediaFactory *
lazy_rtsp_factory_create(UnifiedEngine *engine, CapturePipeline *cap,
                         StreamManager *stream_mgr, const char *lens,
                         const char *tier, BranchConfig *branch_cfg,
                         const char *mount_path, gboolean cairo_enabled) {
    g_info("[RTSP] Creating LazyRTSPFactory for '%s'  "
           "codec=%s  %dx%d @ %d fps  cairo_enabled=%s  ntp_overlay=%s",
           mount_path, branch_cfg->codec, branch_cfg->width, branch_cfg->height, branch_cfg->fps,
           cairo_enabled ? "TRUE" : "FALSE", branch_cfg->ntp_overlay ? "TRUE" : "FALSE");

    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    LazyRTSPData *rtsp = g_new0(LazyRTSPData, 1);
    rtsp->engine     = engine;
    rtsp->capture    = cap;
    rtsp->stream_mgr = stream_mgr;
    g_strlcpy(rtsp->lens,       lens,       sizeof(rtsp->lens));
    g_strlcpy(rtsp->tier,       tier,       sizeof(rtsp->tier));
    g_strlcpy(rtsp->codec,      branch_cfg->codec, sizeof(rtsp->codec));
    rtsp->width         = branch_cfg->width;
    rtsp->height        = branch_cfg->height;
    rtsp->fps           = branch_cfg->fps;
    rtsp->bitrate       = branch_cfg->bitrate;
    rtsp->cairo_enabled = cairo_enabled;
    rtsp->ntp_overlay   = branch_cfg->ntp_overlay;
    g_strlcpy(rtsp->mount_path, mount_path, sizeof(rtsp->mount_path));
    rtsp->refcount = 1;

    g_object_set_qdata_full(G_OBJECT(factory), _lazy_rtsp_quark(), rtsp,
                            (GDestroyNotify)lazy_rtsp_data_unref);
    g_mutex_init(&rtsp->branch_lock);

    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_media_factory_set_protocols(
        factory, GST_RTSP_LOWER_TRANS_TCP | GST_RTSP_LOWER_TRANS_UDP |
                 GST_RTSP_LOWER_TRANS_UDP_MCAST);

    /* Build RTSP launch string */
    char launch[512];
    if (g_strcmp0(tier, "h264") == 0 || g_strcmp0(branch_cfg->codec, "h264") == 0) {
        snprintf(launch, sizeof(launch),
                 "( appsrc name=pay_src is-live=true format=time block=true "
                 "caps=\"video/x-h264,stream-format=byte-stream,alignment=au\" "
                 "! h264parse "
                 "! queue max-size-time=100000000 max-size-bytes=2097152 "
                 "max-size-buffers=0 "
                 "! rtph264pay name=pay0 pt=96 config-interval=1 )");
    } else if (g_strcmp0(tier, "h265") == 0 || g_strcmp0(branch_cfg->codec, "h265") == 0) {
        snprintf(launch, sizeof(launch),
                 "( appsrc name=pay_src is-live=true format=time block=true "
                 "caps=\"video/x-h265,stream-format=byte-stream,alignment=au\" "
                 "! h265parse "
                 "! queue max-size-time=100000000 max-size-bytes=2097152 "
                 "max-size-buffers=0 "
                 "! rtph265pay name=pay0 pt=96 config-interval=1 )");
    } else { /* mjpeg */
        snprintf(launch, sizeof(launch),
                 "( appsrc name=pay_src is-live=true format=time block=true "
                 "caps=\"image/jpeg\" "
                 "! jpegparse "
                 "! queue max-size-time=100000000 max-size-bytes=2097152 "
                 "max-size-buffers=0 "
                 "! rtpjpegpay name=pay0 pt=26 )");
    }
    g_debug("[RTSP] launch string for '%s': %s", mount_path, launch);
    gst_rtsp_media_factory_set_launch(factory, launch);

    g_signal_connect(factory, "media-configure",   G_CALLBACK(_on_media_configure), NULL);
    g_signal_connect(factory, "media-constructed", G_CALLBACK(_on_media_constructed), rtsp);

    return factory;
}

GstRTSPServer *rtsp_server_start(int port) {
    g_info("[RTSP] Starting RTSP server on port %d", port);
    GstRTSPServer *server = gst_rtsp_server_new();
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    gst_rtsp_server_set_service(server, port_str);
    gst_rtsp_server_attach(server, NULL);
    g_info("[RTSP] ✓ Server listening on port %d", port);
    return server;
}

void rtsp_server_dump_all_dots(GstRTSPServer *server, const char *ts) {
    (void)ts;
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    g_object_unref(mounts);
}

void lazy_rtsp_factory_dump_dot(GstRTSPMediaFactory *factory, const char *ts) {
    g_info("[TRACE] lazy_rtsp_factory_dump_dot START for factory %p", factory);
    if (!factory) {
        g_warning("[TRACE]   NULL factory!");
        return;
    }

    LazyRTSPData *rtsp = g_object_get_qdata(G_OBJECT(factory), _lazy_rtsp_quark());
    if (!rtsp) {
        g_warning("[TRACE]   NULL LazyRTSPData!");
        return;
    }

    g_info("[TRACE]   Taking branch_lock for %s...", rtsp->mount_path);
    g_mutex_lock(&(rtsp->branch_lock));
    g_info("[TRACE]   Lock acquired.");

    if (!rtsp->active_media) {
        g_info("[TRACE]   No active media for %s. Skipping.", rtsp->mount_path);
        g_mutex_unlock(&rtsp->branch_lock);
        return;
    }

    GstRTSPMediaStatus media_status = gst_rtsp_media_get_status(rtsp->active_media);
    if (media_status < GST_RTSP_MEDIA_STATUS_PREPARED) {
        g_info("[TRACE]   Media not prepared. Skipping.");
        g_mutex_unlock(&rtsp->branch_lock);
        return;
    }

    GstElement *media_pipeline = gst_rtsp_media_get_element(rtsp->active_media);
    g_mutex_unlock(&rtsp->branch_lock);

    if (media_pipeline && GST_IS_ELEMENT(media_pipeline)) {
        char dot_name[128];
        snprintf(dot_name, sizeof(dot_name), "RTSP_%s_%s_%s",
                 rtsp->lens, rtsp->tier, ts);
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(media_pipeline),
                                  GST_DEBUG_GRAPH_SHOW_ALL, dot_name);
        gst_object_unref(media_pipeline);
    }
    g_info("[TRACE] lazy_rtsp_factory_dump_dot FINISHED for %s", rtsp->mount_path);
}
