/*
 * sys_hls.c — HLS Stream Generator
 * ===================================================
 */

#include "subsystems/sys_hls.h"
#include <gst/app/gstappsrc.h>
#include <stdio.h>
#include <string.h>

struct _HLSGenerator {
    StreamManager *stream_mgr;
    char           lens_name[32];
    char           tier_name[32];
    char           codec_name[16];
    char           output_dir[256];

    GstElement    *pipeline;
    GstElement    *appsrc;
    gulong         handler_id;
    gboolean       is_active;

    guint64        base_pts;
    gboolean       base_pts_set;

    gint64         last_refresh_time;
    GMutex         lock;

    /* Instrumentation */
    volatile gint  buf_pushed;
    volatile gint  buf_skipped;   /* delta frames dropped before first IDR */
    volatile gint  buf_failed;
};

/* ── StreamManager Callback ── */
static void _on_encoded_buffer(GstBuffer *buf, gpointer user_data) {
    HLSGenerator *hls_gen = (HLSGenerator *)user_data;

    g_mutex_lock(&(hls_gen->lock));
    if (!hls_gen->is_active || !hls_gen->appsrc) {
        g_mutex_unlock(&hls_gen->lock);
        gst_buffer_unref(buf);
        return;
    }

    guint64 pts = GST_BUFFER_PTS(buf);
    guint64 dts = GST_BUFFER_DTS(buf);

    if (!hls_gen->base_pts_set) {
        if (!GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) {
            hls_gen->base_pts = (dts != GST_CLOCK_TIME_NONE && dts < pts) ? dts : pts;
            hls_gen->base_pts_set = TRUE;
            g_info("[HLS %s:%s] IDR keyframe found at pts=%" G_GUINT64_FORMAT "ms  "
                   "base_pts=%" G_GUINT64_FORMAT "ms  "
                   "skipped_delta_frames=%d",
                   hls_gen->lens_name, hls_gen->tier_name,
                   pts / GST_MSECOND,
                   hls_gen->base_pts / GST_MSECOND,
                   g_atomic_int_get(&hls_gen->buf_skipped));
        } else {
            g_atomic_int_inc(&hls_gen->buf_skipped);
            int sk = g_atomic_int_get(&hls_gen->buf_skipped);
            if (sk % 30 == 0) {
                g_debug("[HLS %s:%s] Waiting for IDR keyframe — skipped %d delta frames",
                        hls_gen->lens_name, hls_gen->tier_name, sk);
            }
            g_mutex_unlock(&hls_gen->lock);
            gst_buffer_unref(buf);
            return;
        }
    }

    GST_BUFFER_PTS(buf) = pts - hls_gen->base_pts;
    GST_BUFFER_DTS(buf) = (dts != GST_CLOCK_TIME_NONE && dts >= hls_gen->base_pts)
                          ? (dts - hls_gen->base_pts)
                          : GST_CLOCK_TIME_NONE;

    GstFlowReturn flow_ret = gst_app_src_push_buffer(GST_APP_SRC(hls_gen->appsrc), buf);
    if (flow_ret != GST_FLOW_OK) {
        g_atomic_int_inc(&hls_gen->buf_failed);
        g_debug("[HLS %s:%s] Push FAILED: %s  total_fails=%d",
                hls_gen->lens_name, hls_gen->tier_name,
                gst_flow_get_name(flow_ret),
                g_atomic_int_get(&hls_gen->buf_failed));
        gst_buffer_unref(buf);
    } else {
        g_atomic_int_inc(&hls_gen->buf_pushed);
        int pushed = g_atomic_int_get(&hls_gen->buf_pushed);
        if (pushed % 150 == 0) {
            g_debug("[HLS %s:%s] frames=%d  fails=%d  output_dir=%s",
                    hls_gen->lens_name, hls_gen->tier_name,
                    pushed,
                    g_atomic_int_get(&hls_gen->buf_failed),
                    hls_gen->output_dir);
        }
    }
    g_mutex_unlock(&hls_gen->lock);
}

HLSGenerator *hls_generator_new(StreamManager *stream_mgr, const char *lens_name, const char *tier_name,
                                 const char *codec_name, const char *output_dir) {
    g_info("[HLS] hls_generator_new: lens=%s tier=%s codec=%s output_dir=%s",
           lens_name, tier_name, codec_name, output_dir);
    HLSGenerator *hls_gen = g_new0(HLSGenerator, 1);
    hls_gen->stream_mgr = stream_mgr;
    g_strlcpy(hls_gen->lens_name, lens_name, sizeof(hls_gen->lens_name));
    g_strlcpy(hls_gen->tier_name, tier_name, sizeof(hls_gen->tier_name));
    g_strlcpy(hls_gen->codec_name, codec_name, sizeof(hls_gen->codec_name));
    g_strlcpy(hls_gen->output_dir, output_dir, sizeof(hls_gen->output_dir));
    g_mutex_init(&hls_gen->lock);
    hls_generator_refresh(hls_gen);
    return hls_gen;
}

void hls_generator_free(HLSGenerator *hls_gen) {
    if (!hls_gen) return;
    g_info("[HLS %s:%s] Freeing generator  "
           "final_pushed=%d  final_skipped=%d  final_failed=%d",
           hls_gen->lens_name, hls_gen->tier_name,
           g_atomic_int_get(&hls_gen->buf_pushed),
           g_atomic_int_get(&hls_gen->buf_skipped),
           g_atomic_int_get(&hls_gen->buf_failed));
    hls_generator_stop(hls_gen);
    g_mutex_clear(&hls_gen->lock);
    g_free(hls_gen);
}

void hls_generator_start(HLSGenerator *hls_gen) {
    if (hls_gen->is_active) {
        g_debug("[HLS %s:%s] start called but already active", hls_gen->lens_name, hls_gen->tier_name);
        return;
    }
    g_mkdir_with_parents(hls_gen->output_dir, 0755);

    const char *parser_factory = (g_strcmp0(hls_gen->codec_name, "h265") == 0) ? "h265parse" : "h264parse";

    char launch_cmd[1024];
    snprintf(launch_cmd, sizeof(launch_cmd),
             "appsrc name=hls_src is-live=true format=time block=false ! "
             "queue max-size-buffers=30 leaky=downstream ! "
             "%s ! mpegtsmux ! "
             "hlssink location=%s/segment%%05d.ts "
             "playlist-location=%s/playlist.m3u8 "
             "target-duration=1 max-files=5",
             parser_factory, hls_gen->output_dir, hls_gen->output_dir);

    g_info("[HLS %s:%s] Starting pipeline: %s", hls_gen->lens_name, hls_gen->tier_name, launch_cmd);

    GError *error = NULL;
    hls_gen->pipeline = gst_parse_launch(launch_cmd, &error);
    if (error) {
        g_warning("[HLS %s:%s] Pipeline creation FAILED: %s",
                  hls_gen->lens_name, hls_gen->tier_name, error->message);
        g_error_free(error);
        return;
    }

    hls_gen->appsrc = gst_bin_get_by_name(GST_BIN(hls_gen->pipeline), "hls_src");
    if (!hls_gen->appsrc) {
        g_warning("[HLS %s:%s] Could not find 'hls_src' appsrc!", hls_gen->lens_name, hls_gen->tier_name);
    }

    char caps_str[128];
    snprintf(caps_str, sizeof(caps_str),
             "video/x-%s,stream-format=byte-stream,alignment=au",
             (g_strcmp0(hls_gen->codec_name, "h265") == 0) ? "h265" : "h264");
    GstCaps *caps = gst_caps_from_string(caps_str);
    g_object_set(hls_gen->appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    hls_gen->handler_id = stream_manager_subscribe(hls_gen->stream_mgr, hls_gen->lens_name, hls_gen->tier_name,
                                                _on_encoded_buffer, hls_gen);
    
    gst_element_set_state(hls_gen->pipeline, GST_STATE_PLAYING);
    hls_gen->is_active        = TRUE;
    hls_gen->base_pts_set     = FALSE;
    hls_gen->buf_pushed       = 0;
    hls_gen->buf_skipped      = 0;
    hls_gen->buf_failed       = 0;

    g_info("[HLS %s:%s] ✓ Generator started  output=%s",
           hls_gen->lens_name, hls_gen->tier_name, hls_gen->output_dir);
}

void hls_generator_stop(HLSGenerator *hls_gen) {
    if (!hls_gen->is_active) {
        g_debug("[HLS %s:%s] stop called but not active", hls_gen->lens_name, hls_gen->tier_name);
        return;
    }

    stream_manager_unsubscribe(hls_gen->stream_mgr, hls_gen->lens_name, hls_gen->tier_name, hls_gen->handler_id);

    g_mutex_lock(&(hls_gen->lock));
    hls_gen->is_active = FALSE;

    if (hls_gen->pipeline) {
        if (hls_gen->appsrc) {
            gst_app_src_end_of_stream(GST_APP_SRC(hls_gen->appsrc));
            GstBus *bus = gst_element_get_bus(hls_gen->pipeline);
            if (bus) {
                GstMessage *msg = gst_bus_timed_pop_filtered(
                    bus, 500 * GST_MSECOND,
                    GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
                if (msg) {
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                        GError *e = NULL;
                        gst_message_parse_error(msg, &e, NULL);
                        g_warning("[HLS %s:%s] Error during EOS wait: %s",
                                  hls_gen->lens_name, hls_gen->tier_name, e->message);
                        g_error_free(e);
                    }
                    gst_message_unref(msg);
                }
                gst_object_unref(bus);
            }
        }

        gst_element_set_state(hls_gen->pipeline, GST_STATE_NULL);
        if (hls_gen->appsrc) {
            gst_object_unref(hls_gen->appsrc);
            hls_gen->appsrc = NULL;
        }
        gst_object_unref(hls_gen->pipeline);
        hls_gen->pipeline = NULL;
    }
    g_mutex_unlock(&hls_gen->lock);

    g_info("[HLS %s:%s] ✓ Generator stopped", hls_gen->lens_name, hls_gen->tier_name);
}

void hls_generator_refresh(HLSGenerator *hls_gen) {
    if (!hls_gen) return;
    g_mutex_lock(&(hls_gen->lock));
    hls_gen->last_refresh_time = g_get_monotonic_time();
    g_mutex_unlock(&hls_gen->lock);
}

gint64 hls_generator_get_idle_time_sec(HLSGenerator *hls_gen) {
    if (!hls_gen) return 0;
    g_mutex_lock(&(hls_gen->lock));
    gint64 diff = g_get_monotonic_time() - hls_gen->last_refresh_time;
    g_mutex_unlock(&hls_gen->lock);
    return diff / G_TIME_SPAN_SECOND;
}

GstElement *hls_generator_get_pipeline(HLSGenerator *hls_gen) {
    return hls_gen ? hls_gen->pipeline : NULL;
}
