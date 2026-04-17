/*
 * sys_stream.c — Shared Encoder Management
 * =========================================
 *
 * FIXES APPLIED:
 *   [FIX-1] Added DynamicBranch *branch field to ManagedStream.
 *   [FIX-2] Removed duplicate stream_manager_acquire implementation.
 *   [FIX-3] Added gst_object_ref(stream->appsink) before pipeline attachment.
 *   [FIX-4] Subscriber callbacks invoked outside stream->lock to prevent deadlock.
 *   [FIX-5] stream_manager_force_teardown now silences appsink callbacks before
 *            freeing the ManagedStream, preventing a use-after-free crash when
 *            the streaming thread fires _on_new_sample mid-teardown.
 *
 * NAMING IMPROVEMENTS:
 *   - ManagedStream pointer renamed from `ms` to descriptive names per context:
 *       stream_manager_acquire      → stream
 *       stream_manager_release      → found_stream
 *       stream_manager_subscribe    → target_stream
 *       stream_manager_unsubscribe  → target_stream
 *       _on_new_sample              → stream
 *       _managed_stream_new         → new_stream
 *       _managed_stream_free        → dead_stream
 *   - force_teardown lookup result  → doomed_stream
 *   - Subscriber snapshot indices   → snap_idx / cb_idx
 *   - Subscriber count              → subscriber_count
 *   - GstMemory loop vars           → mem_block / mem_idx / memory_block_count
 *   - Subscriber* variable `sub`    → subscriber
 *
 * CPU-COPY OPTIMISATIONS (2025-04):
 *   [OPT-COPY] _on_new_sample: replaced gst_buffer_ref + gst_buffer_make_writable
 *              with gst_buffer_copy_region using FLAGS|TIMESTAMPS|META flags.
 *   [OPT-SNAP] Pre-allocated snapshot array eliminates per-frame malloc.
 *   [OPT-TEE]  build_encoder_elements receives src_w/src_h from the capture
 *              pipeline so it can skip the per-branch G2D when branch resolution
 *              equals the tee output resolution.
 *   [OPT-PATH] cairo_enabled=FALSE routes the branch to the NV12 tee.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "subsystems/sys_stream.h"
#include "../debug_perf.h"
#include "gst_blocks/encoder_builder.h"
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_shared_instance_counter = 0;

/* Maximum simultaneous subscribers per stream.  16 covers RTSP + HLS +
 * recording + spare; expand if needed. */
#define MAX_SUBSCRIBERS 16

typedef struct {
    StreamBufferFunc func;
    gpointer         user_data;
} Subscriber;

typedef struct {
    char             lens[32];
    char             tier[32];
    int              ref_count;
    char             branch_name[64];
    GstElement      *appsink;
    GHashTable      *subscribers;       /* handler_id (gulong) → Subscriber* */
    GMutex           lock;
    StreamManager   *parent;
    CapturePipeline *cap;
    DynamicBranch   *branch;            /* [FIX-1] owns the pipeline branch */

    /* [OPT-SNAP] Pre-allocated snapshot buffer — avoids per-frame malloc */
    Subscriber      *sub_snapshot[MAX_SUBSCRIBERS];

    /* Instrumentation */
    volatile gint    buf_delivered;
    volatile gint    buf_dropped;
} ManagedStream;

struct _StreamManager {
    GHashTable  *streams;               /* "lens:tier" → ManagedStream* */
    GMutex       mgr_lock;
    gulong       next_handler_id;
};

/* Forward declarations */
static GstFlowReturn  _on_new_sample(GstAppSink *sink, gpointer user_data);
static ManagedStream *_managed_stream_new(StreamManager *mgr, const char *lens, const char *tier);
static void           _managed_stream_free(ManagedStream *dead_stream);

/* ── StreamManager Implementation ── */

StreamManager *stream_manager_new(void) {
    g_debug("[SMGR] stream_manager_new");
    StreamManager *mgr = g_new0(StreamManager, 1);
    mgr->streams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)_managed_stream_free);
    g_mutex_init(&mgr->mgr_lock);
    mgr->next_handler_id = 1;
    return mgr;
}

void stream_manager_free(StreamManager *mgr) {
    if (!mgr) return;
    g_debug("[SMGR] stream_manager_free  active_streams=%u",
            g_hash_table_size(mgr->streams));
    g_hash_table_destroy(mgr->streams);
    g_mutex_clear(&mgr->mgr_lock);
    g_free(mgr);
}

/* [FIX-2] Single clean implementation of stream_manager_acquire. */
gboolean stream_manager_acquire(StreamManager *mgr, CapturePipeline *cap,
                                const char *lens, const char *tier,
                                BranchConfig *cfg, gboolean cairo_enabled) {
    g_info("[SMGR] stream_manager_acquire: %s:%s  cairo=%s",
           lens, tier, cairo_enabled ? "TRUE" : "FALSE");

    g_mutex_lock(&(mgr->mgr_lock));
    char *branch_key = g_strdup_printf("%s:%s", lens, tier);
    ManagedStream *stream = g_hash_table_lookup(mgr->streams, branch_key);

    if (stream) {
        stream->ref_count++;
        g_info("[SMGR] Reusing existing stream for '%s' (refs=%d)", branch_key, stream->ref_count);
        g_mutex_unlock(&mgr->mgr_lock);
        g_free(branch_key);
        return TRUE;
    }

    stream = _managed_stream_new(mgr, lens, tier);
    stream->ref_count = 1;
    stream->cap = cap;

    /* [OPT-TEE] Pass tee resolution so encoder_builder can skip the per-branch
     * G2D when branch resolution matches tee output. */
    int enc_count = 0;
    gboolean is_third = (g_strcmp0(tier, "third") == 0);
    GstElement **enc_elements = build_encoder_elements(
        stream->branch_name,
        cfg->codec,
        cfg->fps,
        cfg->w, cfg->h,
        cap->tee_w, cap->tee_h,
        cfg->bitrate,
        cfg->bitrate_mode,
        cairo_enabled,
        cfg->ntp_overlay,
        is_third,
        &enc_count);

    if (!enc_elements || enc_count == 0) {
        g_warning("[SMGR] build_encoder_elements returned NULL for '%s'", branch_key);
        _managed_stream_free(stream);
        g_mutex_unlock(&mgr->mgr_lock);
        g_free(branch_key);
        return FALSE;
    }

    /* Append appsink */
    int total_elements = enc_count + 1;
    GstElement **all_elements = g_new(GstElement *, total_elements);
    memcpy(all_elements, enc_elements, sizeof(GstElement *) * enc_count);
    g_free(enc_elements);

    char sink_name[256];
    snprintf(sink_name, sizeof(sink_name), "shared_sink_%s", stream->branch_name);
    stream->appsink = gst_element_factory_make("appsink", sink_name);
    if (!stream->appsink) {
        g_warning("[SMGR] Failed to create appsink for '%s'", branch_key);
        _managed_stream_free(stream);
        g_mutex_unlock(&mgr->mgr_lock);
        g_free(all_elements);
        g_free(branch_key);
        return FALSE;
    }

    g_object_set(stream->appsink,
                 "emit-signals", TRUE,
                 "drop",         TRUE,
                 "max-buffers",  2,
                 "sync",         FALSE,
                 NULL);

    GstAppSinkCallbacks callbacks = {0};
    callbacks.new_sample = (GstFlowReturn (*)(GstAppSink *, gpointer))_on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(stream->appsink), &callbacks, stream, NULL);

    /* [FIX-3] Extra ref so _managed_stream_free's g_clear_object() doesn't
     * leave GStreamer holding a dangling pointer after the bin removes it. */
    gst_object_ref(stream->appsink);
    all_elements[enc_count] = stream->appsink;

    /* [OPT-TEE] Intelligent Tee Selection:
     * Use NV12 tee ONLY if Cairo and NTP are BOTH disabled. */
    gboolean use_nv12_tee = !(cairo_enabled || cfg->ntp_overlay);
    stream->branch = capture_pipeline_add_branch(cap, stream->branch_name,
                                                 all_elements, total_elements,
                                                 use_nv12_tee);
    g_free(all_elements);

    if (!stream->branch) {
        g_warning("[SMGR] FAILED to add branch '%s' to capture pipeline", stream->branch_name);
        gst_object_unref(stream->appsink);
        _managed_stream_free(stream);
        g_mutex_unlock(&mgr->mgr_lock);
        g_free(branch_key);
        return FALSE;
    }

    g_hash_table_insert(mgr->streams, branch_key, stream);
    g_info("[SMGR] ✓ Created shared encoder branch '%s'  (total active=%u)",
           stream->branch_name, g_hash_table_size(mgr->streams));

    g_mutex_unlock(&mgr->mgr_lock);
    return TRUE;
}

void stream_manager_release(StreamManager *mgr, const char *lens, const char *tier) {
    g_mutex_lock(&(mgr->mgr_lock));
    char *branch_key = g_strdup_printf("%s:%s", lens, tier);
    ManagedStream *found_stream = g_hash_table_lookup(mgr->streams, branch_key);

    if (found_stream) {
        found_stream->ref_count--;
        g_info("[SMGR] release '%s'  refcount=%d  delivered=%d  dropped=%d",
               branch_key, found_stream->ref_count,
               g_atomic_int_get(&found_stream->buf_delivered),
               g_atomic_int_get(&found_stream->buf_dropped));

        if (found_stream->ref_count <= 0) {
            g_info("[SMGR] refcount=0 — triggering teardown for '%s'", branch_key);

            /* Silence the appsink callbacks BEFORE removing from hash table.
             * This guarantees _on_new_sample won't touch found_stream after we free it. */
            if (found_stream->appsink && GST_IS_ELEMENT(found_stream->appsink)) {
                g_object_set(found_stream->appsink, "emit-signals", FALSE, NULL);
                GstAppSinkCallbacks null_callbacks = {0};
                gst_app_sink_set_callbacks(GST_APP_SINK(found_stream->appsink),
                                           &null_callbacks, NULL, NULL);
                gst_element_set_state(found_stream->appsink, GST_STATE_NULL);
            }

            /* Steal the branch name before g_hash_table_remove frees found_stream */
            char branch_name_copy[64];
            g_strlcpy(branch_name_copy, found_stream->branch_name, sizeof(branch_name_copy));
            CapturePipeline *cap_ref = found_stream->cap;

            /* Remove found_stream from the table — this calls _managed_stream_free */
            g_hash_table_remove(mgr->streams, branch_key);

            /* CRITICAL: Drop mgr_lock BEFORE calling capture_pipeline_remove_branch.
             * remove_branch acquires cap->branch_mutex. If we hold mgr_lock here
             * and the streaming thread is in _on_new_sample (holding stream->lock and
             * waiting for mgr_lock), we have an AB/BA deadlock.
             * Releasing mgr_lock first breaks the cycle. */
            g_mutex_unlock(&mgr->mgr_lock);

            if (cap_ref && branch_name_copy[0] != '\0') {
                g_info("[SMGR] Removing GStreamer branch '%s' from pipeline '%s'",
                       branch_name_copy, cap_ref->device);
                capture_pipeline_remove_branch(cap_ref, branch_name_copy);
            }

            g_free(branch_key);
            return; /* mgr_lock already released above */
        }
    } else {
        g_warning("[SMGR] release: stream '%s' not found!", branch_key);
    }

    g_mutex_unlock(&mgr->mgr_lock);
    g_free(branch_key);
}

/* [FIX-5] BUG FIX: Silence appsink callbacks before freeing the ManagedStream.
 *
 * The previous implementation called g_hash_table_remove() directly, which
 * triggered _managed_stream_free() while appsink's "new-sample" signal was
 * still connected.  If the streaming thread fired _on_new_sample() between
 * the hash table removal and the free, it would dereference the freed
 * ManagedStream pointer — a use-after-free crash.
 *
 * The fix mirrors the safe teardown sequence already used in
 * stream_manager_release(): silence the appsink first, THEN remove+free.
 *
 * Note: capture_pipeline_remove_branch() is intentionally NOT called here.
 * The engine (engine_configure_lens in main.c) always calls
 * capture_pipeline_remove_branch() BEFORE calling this function, so the
 * GStreamer branch is already detached from the pipeline by the time we run. */
void stream_manager_force_teardown(StreamManager *mgr, const char *lens, const char *tier) {
    if (!mgr || !lens || !tier) return;

    g_mutex_lock(&(mgr->mgr_lock));
    char *lookup_key = g_strdup_printf("%s:%s", lens, tier);

    ManagedStream *doomed_stream = g_hash_table_lookup(mgr->streams, lookup_key);
    if (doomed_stream) {
        g_info("[SMGR] FORCE TEARDOWN for '%s'", lookup_key);

        /* Silence callbacks BEFORE freeing — prevents use-after-free */
        if (doomed_stream->appsink && GST_IS_ELEMENT(doomed_stream->appsink)) {
            g_object_set(doomed_stream->appsink, "emit-signals", FALSE, NULL);
            GstAppSinkCallbacks null_callbacks = {0};
            gst_app_sink_set_callbacks(GST_APP_SINK(doomed_stream->appsink),
                                       &null_callbacks, NULL, NULL);
            gst_element_set_state(doomed_stream->appsink, GST_STATE_NULL);
        }

        g_hash_table_remove(mgr->streams, lookup_key);
    } else {
        g_debug("[SMGR] force_teardown: '%s' not active, nothing to do", lookup_key);
    }

    g_mutex_unlock(&mgr->mgr_lock);
    g_free(lookup_key);
}

gboolean stream_manager_is_active(StreamManager *mgr, const char *lens, const char *tier) {
    if (!mgr || !lens || !tier) return FALSE;

    g_mutex_lock(&(mgr->mgr_lock));
    char *branch_key = g_strdup_printf("%s:%s", lens, tier);
    gboolean is_active = g_hash_table_contains(mgr->streams, branch_key);
    g_mutex_unlock(&mgr->mgr_lock);
    g_free(branch_key);
    return is_active;
}

gulong stream_manager_subscribe(StreamManager *mgr, const char *lens, const char *tier,
                                StreamBufferFunc func, gpointer user_data) {
    g_mutex_lock(&(mgr->mgr_lock));
    char *branch_key = g_strdup_printf("%s:%s", lens, tier);
    ManagedStream *target_stream = g_hash_table_lookup(mgr->streams, branch_key);
    g_free(branch_key);

    if (!target_stream) {
        g_warning("[SMGR] subscribe: stream '%s:%s' not found — call acquire first",
                  lens, tier);
        g_mutex_unlock(&mgr->mgr_lock);
        return 0;
    }

    g_mutex_lock(&(target_stream->lock));
    gulong handler_id = mgr->next_handler_id++;
    Subscriber *subscriber = g_new0(Subscriber, 1);
    subscriber->func = func;
    subscriber->user_data = user_data;
    g_hash_table_insert(target_stream->subscribers, GUINT_TO_POINTER(handler_id), subscriber);

    g_debug("[SMGR] subscribe '%s:%s'  handler_id=%lu  subscribers=%u",
            lens, tier, handler_id, g_hash_table_size(target_stream->subscribers));
    g_mutex_unlock(&target_stream->lock);
    g_mutex_unlock(&mgr->mgr_lock);
    return handler_id;
}

void stream_manager_unsubscribe(StreamManager *mgr, const char *lens, const char *tier,
                                gulong handler_id) {
    g_mutex_lock(&(mgr->mgr_lock));
    char *branch_key = g_strdup_printf("%s:%s", lens, tier);
    ManagedStream *target_stream = g_hash_table_lookup(mgr->streams, branch_key);
    g_free(branch_key);

    if (target_stream) {
        g_mutex_lock(&(target_stream->lock));
        gboolean removed = g_hash_table_remove(target_stream->subscribers,
                                               GUINT_TO_POINTER(handler_id));
        g_debug("[SMGR] unsubscribe '%s:%s'  handler_id=%lu  removed=%s  remaining=%u",
                lens, tier, handler_id,
                removed ? "YES" : "NO",
                g_hash_table_size(target_stream->subscribers));
        g_mutex_unlock(&target_stream->lock);
    } else {
        g_warning("[SMGR] unsubscribe: stream '%s:%s' not found", lens, tier);
    }
    g_mutex_unlock(&mgr->mgr_lock);
}

const char *stream_manager_get_active_branch_name(StreamManager *mgr,
                                                  const char *lens,
                                                  const char *tier) {
    if (!mgr) return NULL;
    g_mutex_lock(&(mgr->mgr_lock));
    char *branch_key = g_strdup_printf("%s:%s", lens, tier);
    ManagedStream *found_stream = g_hash_table_lookup(mgr->streams, branch_key);
    g_free(branch_key);
    const char *active_name = found_stream ? found_stream->branch_name : NULL;
    g_mutex_unlock(&mgr->mgr_lock);
    return active_name;
}

/* ── Private Helpers ── */

/*
 * [FIX-4]  Subscriber callbacks invoked outside stream->lock (deadlock prevention).
 * [OPT-COPY] Zero-copy buffer delivery via gst_buffer_copy_region header copy.
 * [OPT-SNAP] Pre-allocated snapshot array eliminates per-frame malloc.
 */
static GstFlowReturn _on_new_sample(GstAppSink *sink, gpointer user_data) {
    ManagedStream *stream = (ManagedStream *)user_data;

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        g_debug("[SMGR-BUF] NULL sample from appsink (EOS/flush?)");
        return GST_FLOW_OK;
    }

    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (!buf) {
        g_atomic_int_inc(&stream->buf_dropped);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    /* Step 1: snapshot subscriber list under lock (no heap alloc) */
    g_mutex_lock(&(stream->lock));

    if (_perf_debug_enabled()) {
        g_atomic_int_inc(&stream->buf_delivered);
        int total = g_atomic_int_get(&stream->buf_delivered);
        if (total % 300 == 0) {
            g_debug("[SMGR-BUF] stream '%s:%s'  delivered=%d  dropped=%d  subscribers=%u",
                    stream->lens, stream->tier, total,
                    g_atomic_int_get(&stream->buf_dropped),
                    g_hash_table_size(stream->subscribers));
        }
    }

    /* [OPT-SNAP] Fill pre-allocated snapshot array — zero malloc */
    guint subscriber_count = g_hash_table_size(stream->subscribers);
    if (subscriber_count > MAX_SUBSCRIBERS) subscriber_count = MAX_SUBSCRIBERS;
    guint snap_idx = 0;
    if (subscriber_count > 0) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init(&iter, stream->subscribers);
        while (g_hash_table_iter_next(&iter, NULL, &val) && snap_idx < subscriber_count)
            stream->sub_snapshot[snap_idx++] = (Subscriber *)val;
    }

    g_mutex_unlock(&stream->lock);

    /* Step 2: invoke callbacks WITHOUT holding stream->lock */
    for (guint cb_idx = 0; cb_idx < snap_idx; cb_idx++) {
        Subscriber *subscriber = stream->sub_snapshot[cb_idx];
        if (!subscriber || !subscriber->func) continue;

        /* [OPT-COPY] True zero-copy: create a new GstBuffer header (copies
         * only flags/timestamps/meta — ~200 bytes) then share each GstMemory
         * block by reference (gst_memory_ref, no pixel data touched).
         *
         * Correct pattern: copy header metadata only, then append a ref to
         * each GstMemory block so the backing bitstream is shared. */
        GstBuffer *header_copy = gst_buffer_copy_region(
            buf,
            GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META,
            0, gst_buffer_get_size(buf));

        if (header_copy) {
            /* Share each GstMemory block by reference — zero pixel copy */
            guint memory_block_count = gst_buffer_n_memory(buf);
            for (guint mem_idx = 0; mem_idx < memory_block_count; mem_idx++) {
                GstMemory *mem_block = gst_buffer_peek_memory(buf, mem_idx);
                gst_buffer_append_memory(header_copy, gst_memory_ref(mem_block));
            }

            /* subscriber->func (e.g. _on_encoded_buffer in sys_rtsp.c) calls
             * gst_app_src_push_buffer() which takes OWNERSHIP of `header_copy`.
             * Do NOT unref after the call — that would be a double-free. */
            subscriber->func(header_copy, subscriber->user_data);
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static ManagedStream *_managed_stream_new(StreamManager *mgr, const char *lens, const char *tier) {
    ManagedStream *new_stream = g_new0(ManagedStream, 1);
    new_stream->parent = mgr;
    g_strlcpy(new_stream->lens, lens, sizeof(new_stream->lens));
    g_strlcpy(new_stream->tier, tier, sizeof(new_stream->tier));

    int instance_id = g_atomic_int_add(&g_shared_instance_counter, 1);
    snprintf(new_stream->branch_name, sizeof(new_stream->branch_name),
             "shared_%s_%s_%d", lens, tier, instance_id);

    new_stream->subscribers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                    NULL, g_free);
    g_mutex_init(&new_stream->lock);

    g_debug("[SMGR] ManagedStream created: '%s:%s'  branch='%s'",
            lens, tier, new_stream->branch_name);
    return new_stream;
}

static void _managed_stream_free(ManagedStream *dead_stream) {
    if (!dead_stream) return;
    g_info("[SMGR] Freeing ManagedStream '%s:%s'  delivered=%d  dropped=%d  subscribers=%u",
           dead_stream->lens, dead_stream->tier,
           g_atomic_int_get(&dead_stream->buf_delivered),
           g_atomic_int_get(&dead_stream->buf_dropped),
           g_hash_table_size(dead_stream->subscribers));

    g_mutex_lock(&(dead_stream->lock));
    g_hash_table_destroy(dead_stream->subscribers);
    g_mutex_unlock(&dead_stream->lock);
    g_mutex_clear(&dead_stream->lock);

    /* [FIX-3] Release the extra ref taken before handing appsink to pipeline.
     * NOTE: capture_pipeline_remove_branch is NOT called here.
     * It is called in stream_manager_release() AFTER dropping mgr_lock,
     * to avoid the AB/BA deadlock between mgr_lock and branch_mutex. */
    g_clear_object(&dead_stream->appsink);

    g_free(dead_stream);
}
