/*
 * recording.c — Recording Branch Implementation
 * ================================================
 */

#include "subsystems/sys_recording.h"
#include "subsystems/sys_capture.h"
#include "gst_blocks/encoder_builder.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "engine_types.h"

static int g_rec_instance_counter = 0;


RecordingBranch *recording_branch_new(UnifiedEngine *engine, CapturePipeline *capture_pipe,
                                       const char *lens, const char *tier,
                                       const char *output_dir, BranchConfig *recording_cfg,
                                       const char *mode, int idle_timeout, int max_segment_sec)
{
    RecordingBranch *recording_branch = g_new0(RecordingBranch, 1);
    recording_branch->engine = engine;
    recording_branch->capture = capture_pipe;
    g_strlcpy(recording_branch->lens, lens, sizeof(recording_branch->lens));
    g_strlcpy(recording_branch->tier, tier, sizeof(recording_branch->tier));
    g_strlcpy(recording_branch->output_dir, output_dir, sizeof(recording_branch->output_dir));
    g_strlcpy(recording_branch->mode, mode ? mode : "continuous", sizeof(recording_branch->mode));
    recording_branch->idle_timeout = idle_timeout > 0 ? idle_timeout : 60;
    recording_branch->max_segment_sec = max_segment_sec > 0 ? max_segment_sec : 300;

    /* Deep copy config values */
    if (recording_cfg) {
        g_strlcpy(recording_branch->codec, recording_cfg->codec, sizeof(recording_branch->codec));
        g_strlcpy(recording_branch->resolution, recording_cfg->resolution, sizeof(recording_branch->resolution));
        recording_branch->width = recording_cfg->width;
        recording_branch->height = recording_cfg->height;
        recording_branch->fps = recording_cfg->fps;
        recording_branch->bitrate = recording_cfg->bitrate;
        recording_branch->ntp_overlay = recording_cfg->ntp_overlay;
    }

    snprintf(recording_branch->branch_name, sizeof(recording_branch->branch_name), "rec_%s_%s", lens, tier);
    g_mutex_init(&recording_branch->rec_lock);
    return recording_branch;
}

/* Helper to brutally force async handling on elements */
static void force_async(GstElement *engine) {
    if (!engine) return;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(engine), "async-handling")) {
        g_object_set(engine, "async-handling", TRUE, NULL);
    }
}

static gboolean _start_recording_branch(RecordingBranch *recording_branch)
{
    if (!recording_branch->capture) {
        g_warning("Recording: No capture for %s", recording_branch->lens);
        return FALSE;
    }

    g_mkdir_with_parents(recording_branch->output_dir, 0755);

    char uid[128];
    int instance_id = g_atomic_int_add(&g_rec_instance_counter, 1);
    snprintf(uid, sizeof(uid), "rec_%s_%s_%d", recording_branch->lens, recording_branch->tier, instance_id);
    
    /* Store unique name for Capture Pipeline management */
    g_strlcpy(recording_branch->branch_name, uid, sizeof(recording_branch->branch_name));
    
    char name[256];

    /* ── Build recording chain directly (MATCHING RTSP EXACTLY minus g2d) ──
     * queue → videorate → fps_capsfilter → encoder → parser → splitmuxsink
     * NO G2D! We take the exact output format/resolution from main pipeline.
     */
    GstElement *elems[14];
    int n = 0;
    int effective_fps = recording_branch->fps > 0 ? recording_branch->fps : 30;

    /* 1. Queue — Matches encoder_builder.c exactly */
    snprintf(name, sizeof(name), "q_%s", uid);
    GstElement *q = gst_element_factory_make("queue", name);
    g_object_set(q,
                 "max-size-buffers", 2,
                 "max-size-bytes",   0,
                 "max-size-time",    (guint64)1000000000, /* 1 second */
                 "leaky",            2,                  /* downstream leaky */
                 NULL);
    force_async(q);
    elems[n++] = q;

    /* 1.5. Hardware Scaler (NV12 → NV12 at target resolution)
     * Recording attaches to capture_tee (NV12). G2D handles NV12→NV12
     * resolution scaling in hardware. No format conversion needed —
     * v4l2h264enc accepts NV12 directly. */
    const char *scaler_factory = gst_element_factory_find("imxvideoconvert_g2d") ? "imxvideoconvert_g2d" : "videoconvert";
    snprintf(name, sizeof(name), "rec_scale_%s", uid);
    GstElement *scaler = gst_element_factory_make(scaler_factory, name);
    force_async(scaler);
    elems[n++] = scaler;

    /* 1.6. Resolution capsfilter (NV12 at target WxH) */
    snprintf(name, sizeof(name), "rec_res_%s", uid);
    GstElement *res_filter = gst_element_factory_make("capsfilter", name);
    char caps_str[256];
    int width = recording_branch->width > 0 ? recording_branch->width : 1920;
    int height = recording_branch->height > 0 ? recording_branch->height : 1080;
    snprintf(caps_str, sizeof(caps_str), "video/x-raw,width=%d,height=%d", width, height);
    GstCaps *res_caps = gst_caps_from_string(caps_str);
    g_object_set(res_filter, "caps", res_caps, NULL);
    gst_caps_unref(res_caps);
    force_async(res_filter);
    elems[n++] = res_filter;

    /* 1.7. NTP Clock Overlay — NOT supported on NV12 pipeline.
     * clockoverlay requires BGRx/RGBA input. Log warning if requested. */
    if (recording_branch->ntp_overlay) {
        g_warning("Recording %s/%s: clockoverlay not supported on NV12 pipeline — skipping.",
                  recording_branch->lens, recording_branch->tier);
    }

    /* 2. Videorate — Matches encoder_builder.c exactly */
    snprintf(name, sizeof(name), "rec_rate_%s", uid);
    GstElement *rate = gst_element_factory_make("videorate", name);
    g_object_set(rate,
                 "drop-only",      TRUE,
                 "max-rate",        effective_fps,
                 "skip-to-first",  TRUE,
                 NULL);
    force_async(rate);
    elems[n++] = rate;

    /* 3. FPS Capsfilter — enforce framerate for v4l2h264enc */
    snprintf(name, sizeof(name), "fps_caps_%s", uid);
    GstElement *fps_filter = gst_element_factory_make("capsfilter", name);
    char fps_str[128];
    snprintf(fps_str, sizeof(fps_str), "video/x-raw,framerate=%d/1", effective_fps);
    GstCaps *fps_caps = gst_caps_from_string(fps_str);
    g_object_set(fps_filter, "caps", fps_caps, NULL);
    gst_caps_unref(fps_caps);
    force_async(fps_filter);
    elems[n++] = fps_filter;

    /* 4. Encoder — Matches encoder_builder.c explicitly */
    snprintf(name, sizeof(name), "enc_%s", uid);
    GstElement *enc = NULL;
    const char *codec = recording_branch->codec;
    int bitrate = recording_branch->bitrate;

    if (g_strcmp0(codec, "h265") == 0) {
        const char *h265_encoders[] = {"v4l2h265enc", "imxvpuh265enc", "v4l2slh265enc", NULL};
        for (int i = 0; h265_encoders[i]; i++) {
            if (gst_element_factory_find(h265_encoders[i])) {
                enc = gst_element_factory_make(h265_encoders[i], name);
                break;
            }
        }
        if (!enc) {
            enc = gst_element_factory_make("x265enc", name);
            if (enc) g_object_set(enc, "speed-preset", 1, "tune", 4, NULL);
        }
    } else { /* h264 */
        const char *h264_encoders[] = {"v4l2h264enc", "imxvpuh264enc", NULL};
        for (int i = 0; h264_encoders[i]; i++) {
            if (gst_element_factory_find(h264_encoders[i])) {
                enc = gst_element_factory_make(h264_encoders[i], name);
                break;
            }
        }
        if (!enc) {
            enc = gst_element_factory_make("x264enc", name);
            if (enc) {
                g_object_set(enc, "speed-preset", 1, "tune", 4,
                             "key-int-max", effective_fps, NULL);
            }
        }
    }

    if (enc) {
        GstElementFactory *factory = gst_element_get_factory(enc);
        const char *fname = factory
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "";
        if (strstr(fname, "v4l2")) {
            GstStructure *ec = gst_structure_new("controls",
                "video_bitrate", G_TYPE_INT, bitrate > 0 ? bitrate : 4000000,
                "video_gop_size", G_TYPE_INT, effective_fps, NULL);
            g_object_set(enc, "extra-controls", ec, NULL);
            gst_structure_free(ec);
        } else if (strstr(fname, "x264") || strstr(fname, "x265")) {
            if (bitrate > 0) g_object_set(enc, "bitrate", bitrate / 1000, NULL);
        }
        force_async(enc);
        elems[n++] = enc;
    }

    /* 5. Identity Firewall */
    snprintf(name, sizeof(name), "ident_%s", uid);
    GstElement *ident = gst_element_factory_make("identity", name);
    g_object_set(ident, "drop-allocation", TRUE, NULL);
    force_async(ident);
    elems[n++] = ident;

    /* 6. Parser */
    snprintf(name, sizeof(name), "parse_%s", uid);
    GstElement *parser = gst_element_factory_make(
        g_strcmp0(codec, "h265") == 0 ? "h265parse" : "h264parse", name);
    force_async(parser);
    elems[n++] = parser;
    
    snprintf(name, sizeof(name), "post_enc_q_%s", uid);
    GstElement *q2 = gst_element_factory_make("queue", name);
    g_object_set(q2, "max-size-time", (guint64)30000000000ULL, 
                 "max-size-buffers", 0, "max-size-bytes", 100000000, NULL);
    force_async(q2);
    elems[n++] = q2;

    /* 7. splitmuxsink with mp4mux */
    snprintf(name, sizeof(name), "splitmux_%s", uid);
    GstElement *mux = gst_element_factory_make("splitmuxsink", name);

    char location[512];
    snprintf(location, sizeof(location), "%s/%s_%s_%s_%%05d.mp4",
             recording_branch->output_dir, recording_branch->lens, recording_branch->tier, uid);
    g_object_set(mux,
                 "location",       location,
                 "max-size-time",  (guint64)(recording_branch->max_segment_sec * GST_SECOND),
                 "async-finalize", TRUE,
                 "send-keyframe-requests", FALSE,
                 NULL);
    force_async(mux);

    GstElement *mp4mux = gst_element_factory_make("mp4mux", NULL);
    if (mp4mux) {
        force_async(mp4mux);
        g_object_set(mp4mux,
                     "faststart", TRUE,
                     "reserved-max-duration", (guint64)7200 * GST_SECOND,
                     "reserved-moov-update-period", (guint64)GST_SECOND,
                     NULL);
        g_object_set(mux, "muxer", mp4mux, NULL);
    }
    elems[n++] = mux;

    /* ── Attach as dynamic branch ── */
    GstElement **all = g_new(GstElement*, n);
    memcpy(all, elems, sizeof(GstElement*) * n);

    DynamicBranch *target_branch = capture_pipeline_add_branch(recording_branch->capture, recording_branch->branch_name,
                                                     all, n, TRUE);
    g_free(all);

    if (!target_branch) {
        g_warning("Recording: Failed to attach branch for %s/%s", recording_branch->lens, recording_branch->tier);
        return FALSE;
    }

    recording_branch->running = TRUE;
    g_info("Recording started: %s → %s", recording_branch->branch_name, location);
    return TRUE;
}

gboolean recording_branch_start(RecordingBranch *recording_branch)
{
    g_mutex_lock(&recording_branch->rec_lock);
    if (recording_branch->running) { g_mutex_unlock(&recording_branch->rec_lock); return TRUE; }

    if (g_strcmp0(recording_branch->mode, "event") == 0) {
        recording_branch->armed = TRUE;
        g_info("Recording armed (event mode): %s/%s — waiting for AI events",
               recording_branch->lens, recording_branch->tier);
        g_mutex_unlock(&recording_branch->rec_lock);
        return TRUE;
    }

    if (g_strcmp0(recording_branch->mode, "scheduled") == 0) {
        recording_branch->armed = TRUE;
        g_info("Recording armed (scheduled mode): %s/%s — waiting for schedule",
               recording_branch->lens, recording_branch->tier);
        g_mutex_unlock(&recording_branch->rec_lock);
        return TRUE;
    }

    gboolean start_success = _start_recording_branch(recording_branch);
    g_mutex_unlock(&recording_branch->rec_lock);
    return start_success;
}

/* ── Scheduler Overlap Checker ── */
gboolean recording_is_overlapping(RecordingBranch *branch_a, RecordingBranch *branch_b) {
    /* If either is not scheduled (Continuous/Event), they overlap with EVERYTHING */
    if (g_strcmp0(branch_a->mode, "scheduled") != 0 || g_strcmp0(branch_b->mode, "scheduled") != 0) {
        return TRUE;
    }

    if (g_strcmp0(branch_a->sched.type, "once") == 0 && g_strcmp0(branch_b->sched.type, "once") == 0) {
        /* Both are once: standard window intersection */
        return (branch_a->sched.real_start < branch_b->sched.real_end && branch_b->sched.real_start < branch_a->sched.real_end);
    } 
    
    if (g_strcmp0(branch_a->sched.type, "daily") == 0 && g_strcmp0(branch_b->sched.type, "daily") == 0) {
        /* Both are daily: check seconds-from-midnight window intersection */
        int as = branch_a->sched.daily_start_sec;
        int ae = branch_a->sched.daily_end_sec;
        int bs = branch_b->sched.daily_start_sec;
        int be = branch_b->sched.daily_end_sec;

        /* Handle midnight wrap-around */
        if (ae < as) ae += 86400;
        if (be < bs) be += 86400;

        return (as < be && bs < ae);
    }

    /* Mixed: Once vs Daily */
    RecordingBranch *once_branch = (g_strcmp0(branch_a->sched.type, "once") == 0) ? branch_a : branch_b;
    RecordingBranch *daily_branch = (once_branch == branch_a) ? branch_b : branch_a;

    struct tm *tm_once = localtime(&once_branch->sched.real_start);
    int ds = daily_branch->sched.daily_start_sec;
    int de = daily_branch->sched.daily_end_sec;
    if (de < ds) de += 86400;

    int os = tm_once->tm_hour * 3600 + tm_once->tm_min * 60 + tm_once->tm_sec;
    int duration = (int)(once_branch->sched.real_end - once_branch->sched.real_start);
    int oe = os + duration;

    return (os < de && ds < oe);
}

/* ── Scheduled Event Helper ── */
static gboolean _check_scheduled_status(RecordingBranch *recording_branch, time_t now) {
    if (g_strcmp0(recording_branch->sched.type, "once") == 0) {
        if (recording_branch->sched.real_start > 0 && now >= recording_branch->sched.real_start && now < recording_branch->sched.real_end) {
            return TRUE;
        }
    } else if (g_strcmp0(recording_branch->sched.type, "daily") == 0) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        int current_sec = tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec;
        if (recording_branch->sched.daily_start_sec < recording_branch->sched.daily_end_sec) {
            return (current_sec >= recording_branch->sched.daily_start_sec && current_sec < recording_branch->sched.daily_end_sec);
        } else {
            return (current_sec >= recording_branch->sched.daily_start_sec || current_sec < recording_branch->sched.daily_end_sec);
        }
    }
    return FALSE;
}

/* ── The Robust Scheduler Loop (365-Day Thread) ── */

gpointer recording_scheduler_loop(gpointer data) {
    UnifiedEngine *engine = (UnifiedEngine *)data;
    g_info("[SCHED] Robust Scheduler Thread Started.");

    g_mutex_lock(&engine->scheduler_mutex);
    while (engine->scheduler_running) {
        time_t now = time(NULL);
        time_t next_event = 0;
        gboolean any_pruned = FALSE;

        /* Iterate all recordings to find transitions and next sleep duration */
        GHashTableIter iter;
        gpointer iter_key, iter_recording;
        g_hash_table_iter_init(&iter, engine->recordings);

        while (g_hash_table_iter_next(&iter, &iter_key, &iter_recording)) {
            RecordingBranch *recording_branch = (RecordingBranch *)iter_recording;

            /* ── A. AUTO-PRUNING: CLEANUP FINISHED "ONCE" JOBS ── */
            if (recording_branch->finished) {
                g_info("[SCHED] Auto-pruning finished task: %s/%s", recording_branch->lens, recording_branch->tier);
                g_hash_table_iter_remove(&iter);
                /* Note: hash table will call recording_branch_free automatically */
                any_pruned = TRUE;
                continue;
            }

            /* ── B. SCHEDULED MODE CHECK ── */
            if (g_strcmp0(recording_branch->mode, "scheduled") == 0) {
                gboolean should_be_running = _check_scheduled_status(recording_branch, now);
                if (g_strcmp0(recording_branch->sched.type, "once") == 0) {
                    g_debug("[SCHED] %s/%s: now=%ld start=%ld end=%ld should_run=%d running=%d",
                            recording_branch->lens, recording_branch->tier, (long)now,
                            (long)recording_branch->sched.real_start, (long)recording_branch->sched.real_end,
                            should_be_running, recording_branch->running);
                }

                if (should_be_running && !recording_branch->running) {
                    /* CATCH-UP Logic: If we are midway, only start if > 10s remains */
                    if (g_strcmp0(recording_branch->sched.type, "once") == 0) {
                        if (now < recording_branch->sched.real_end - 10) {
                            g_info("[SCHED] Starting MID-SCHEDULE task for %s/%s", recording_branch->lens, recording_branch->tier);
                            g_mutex_lock(&recording_branch->rec_lock);
                            _start_recording_branch(recording_branch);
                            g_mutex_unlock(&recording_branch->rec_lock);
                        } else {
                            g_info("[SCHED] Skipping task %s/%s - too close to end time.", recording_branch->lens, recording_branch->tier);
                            recording_branch->finished = TRUE;
                        }
                    } else {
                        g_info("[SCHED] Starting scheduled task for %s/%s", recording_branch->lens, recording_branch->tier);
                        g_mutex_lock(&recording_branch->rec_lock);
                        _start_recording_branch(recording_branch);
                        g_mutex_unlock(&recording_branch->rec_lock);
                    }
                } else if (!should_be_running && recording_branch->running) {
                    g_info("[SCHED] Stopping completed task for %s/%s", recording_branch->lens, recording_branch->tier);
                    recording_branch_stop(recording_branch);
                    if (g_strcmp0(recording_branch->sched.type, "once") == 0) recording_branch->finished = TRUE;
                }

                /* Calculate Next boundary for Scheduler Sleep */
                time_t branch_next = 0;
                if (g_strcmp0(recording_branch->sched.type, "once") == 0) {
                    if (now < recording_branch->sched.real_start) branch_next = recording_branch->sched.real_start;
                    else if (now < recording_branch->sched.real_end) branch_next = recording_branch->sched.real_end;
                } else {
                    struct tm tm_tmp;
                    localtime_r(&now, &tm_tmp);
                    int cur = tm_tmp.tm_hour * 3600 + tm_tmp.tm_min * 60 + tm_tmp.tm_sec;
                    int ds = recording_branch->sched.daily_start_sec;
                    int de = recording_branch->sched.daily_end_sec;

                    if (ds < de) { /* Same-day window: 09:00 - 17:00 */
                        if (cur < ds) branch_next = now + (ds - cur);
                        else if (cur < de) branch_next = now + (de - cur);
                        else branch_next = now + (86400 - cur + ds);
                    } else { /* Midnight cross window: 22:00 - 02:00 */
                        if (cur < de) branch_next = now + (de - cur);      /* Finish today */
                        else if (cur < ds) branch_next = now + (ds - cur); /* Start today */
                        else branch_next = now + (86400 - cur + de);      /* Finish tomorrow */
                    }
                }
                if (branch_next > 0 && (next_event == 0 || branch_next < next_event)) {
                    next_event = branch_next;
                }
            }
        }

        /* ── D. AUTO-SAVE: PERSIST CONFIG IF ANYTHING WAS PURGED ── */
        if (any_pruned) {
            engine_persist_state(engine);
        }

        /* Determining wait duration (High-Precision Monotonic Wait) */
        gint64 wait_ms = 60000; /* Default 60s sanity check */
        if (next_event > 0) {
            time_t diff = next_event - now;
            if (diff > 0) {
                /* Wake up exactly at the event (clamped to 5 min max for safety) */
                wait_ms = MIN(300000, (gint64)diff * 1000);
            }
        }

        /* Deep Sleep using Monotonic Time to prevent Epoch/NTP skew issues */
        gint64 end_time = g_get_monotonic_time() + (wait_ms * 1000);
        g_cond_wait_until(&engine->scheduler_cond, &engine->scheduler_mutex, end_time);
    }
    g_mutex_unlock(&engine->scheduler_mutex);
    return NULL;
}

void recording_scheduler_stop(UnifiedEngine *engine) {
    if (!engine || !engine->scheduler_running) return;

    g_info("[SCHED] Stopping scheduler thread...");
    g_mutex_lock(&engine->scheduler_mutex);
    engine->scheduler_running = FALSE;
    g_cond_signal(&engine->scheduler_cond);
    g_mutex_unlock(&engine->scheduler_mutex);

    if (engine->scheduler_thread) {
        g_thread_join(engine->scheduler_thread);
        engine->scheduler_thread = NULL;
    }
    g_info("[SCHED] Scheduler thread joined.");
}


static gboolean _on_event_timeout(gpointer data) {
    RecordingBranch *recording_branch = (RecordingBranch *)data;
    g_info("[AI-REC] Watchdog timeout reached for %s/%s (120s idle). Stopping...", recording_branch->lens, recording_branch->tier);
    recording_branch_stop(recording_branch);
    return G_SOURCE_REMOVE;
}
static gboolean _poke_on_main(gpointer data)
{
    RecordingBranch *recording_branch = (RecordingBranch *)data;
    recording_branch_poke(recording_branch);
    return G_SOURCE_REMOVE;
}

void recording_branch_poke(RecordingBranch *recording_branch)
{
    if (g_strcmp0(recording_branch->mode, "event") != 0) return;

    /* CRITICAL: Delegate entire logic to main thread.
     * ZMQ thread (AI Alerts) calls this, but GStreamer pipeline modification 
     * must happen on the Main Loop thread to avoid race conditions. */
    if (!g_main_context_is_owner(NULL)) {
        g_main_context_invoke(NULL, _poke_on_main, recording_branch);
        return;
    }

    g_mutex_lock(&recording_branch->rec_lock);
    
    /* 1. If not running, start the pipeline immediately */
    if (!recording_branch->running) {
        if (_start_recording_branch(recording_branch)) {
            g_info("[AI-REC] Event recording triggered by alert: %s/%s", recording_branch->lens, recording_branch->tier);
        } else {
            g_warning("[AI-REC] FAILED to start event recording for %s/%s", recording_branch->lens, recording_branch->tier);
        }
    }

    /* 2. Reset the Inactivity Watchdog */
    if (recording_branch->event_timeout_id > 0) {
        g_source_remove(recording_branch->event_timeout_id);
    }
    
    int idle = recording_branch->idle_timeout > 0 ? recording_branch->idle_timeout : 120;
    recording_branch->event_timeout_id = g_timeout_add_seconds(idle, _on_event_timeout, recording_branch);
    
    g_mutex_unlock(&recording_branch->rec_lock);
}

void recording_branch_stop(RecordingBranch *recording_branch)
{
    g_mutex_lock(&recording_branch->rec_lock);
    if (!recording_branch->running) {
        recording_branch->armed = FALSE;
        g_mutex_unlock(&recording_branch->rec_lock);
        return;
    }
    recording_branch->running = FALSE;
    recording_branch->armed = FALSE;

    if (recording_branch->event_timeout_id > 0) {
        g_source_remove(recording_branch->event_timeout_id);
        recording_branch->event_timeout_id = 0;
    }
    recording_branch->deadline = 0.0;
    g_mutex_unlock(&recording_branch->rec_lock);

    if (recording_branch->capture) {
        capture_pipeline_remove_branch(recording_branch->capture, recording_branch->branch_name);
    }
    g_info("Recording stopped: %s", recording_branch->branch_name);
}

void recording_branch_free(RecordingBranch *recording_branch)
{
    if (!recording_branch) return;
    recording_branch_stop(recording_branch);
    g_mutex_clear(&recording_branch->rec_lock);
    g_free(recording_branch);
}
