/*
 * sys_capture.c — Capture Pipeline + Dynamic Branch Management
 * ==============================================================
 *
 * CONSOLIDATED VERSION:
 *   - Automatic memory negotiation (defaulting to System Memory for stability).
 *   - Fixes for syntax, missing definitions, and DOT dump paths applied.
 *   - Robust AI SHM and dynamic branch teardown logic included.
 */

#include "subsystems/sys_capture.h"
#include <cairo.h>
#include <stdio.h>
#include <string.h>

static void dynamic_branch_free(gpointer data);

/* ══════════════════════════════════════════════════════════════
 *  Cairo Drawing Handler
 * ══════════════════════════════════════════════════════════════ */

/* Maximum bounding boxes rendered per frame. Hard capture_pipe prevents runaway
 * allocation; anything beyond 32 detections is noise on an edge camera. */
#define CAIRO_MAX_COORDS 32

static void _on_cairo_draw(GstElement *overlay, cairo_t *cr, guint64 timestamp,
                           guint64 duration, gpointer user_data) {
  (void)timestamp;
  (void)duration;

  /* Gate 1 — cheapest possible exit. */
  if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(overlay), "overlay_enabled")))
    return;

  CapturePipeline *capture_pipe = (CapturePipeline *)user_data;

  /* Take a LOCAL SNAPSHOT of AI state under the lock. */
  g_mutex_lock(&capture_pipe->ai.mutex);
  int num_coords = capture_pipe->ai.num_coords;
  if (num_coords > CAIRO_MAX_COORDS)
    num_coords = CAIRO_MAX_COORDS;
  int ml_w = capture_pipe->ai.ml_w > 0 ? capture_pipe->ai.ml_w : 640;
  int ml_h = capture_pipe->ai.ml_h > 0 ? capture_pipe->ai.ml_h : 360;

  /* Gate 2 — zero-detection fast path. */
  if (num_coords == 0 || !capture_pipe->ai.coords) {
    g_mutex_unlock(&capture_pipe->ai.mutex);
    return;
  }

  AICoord local_coords[CAIRO_MAX_COORDS];
  memcpy(local_coords, capture_pipe->ai.coords,
         sizeof(AICoord) * (guint)num_coords);
  g_mutex_unlock(&capture_pipe->ai.mutex);

  int current_width =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(overlay), "branch_width"));
  int current_height =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(overlay), "branch_height"));
  if (current_width == 0)
    current_width = 1920;
  if (current_height == 0)
    current_height = 1080;

  double scale_x = (double)current_width / ml_w;
  double scale_y = (double)current_height / ml_h;

  /* Set font properties ONCE per draw call. */
  cairo_set_line_width(cr, 3.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 24.0 * ((double)current_height / 1080.0));

  for (int i = 0; i < num_coords; i++) {
    AICoord *coord_ptr = &local_coords[i];
    double x = coord_ptr->left * scale_x;
    double y = coord_ptr->top * scale_y;
    double bw = (coord_ptr->right - coord_ptr->left) * scale_x;
    double bh = (coord_ptr->bottom - coord_ptr->top) * scale_y;

    cairo_set_source_rgb(cr, 0.0, 1.0, 0.0);
    cairo_rectangle(cr, x, y, bw, bh);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 1.0, 1.0, 0.0);
    cairo_move_to(cr, x, y - 5.0 > 0.0 ? y - 5.0 : 20.0);
    char label_text[64];
    snprintf(label_text, sizeof(label_text), "%s (%.0f%%)", coord_ptr->label,
             coord_ptr->conf * 100.0);
    cairo_show_text(cr, label_text);
  }
}

/* ══════════════════════════════════════════════════════════════
 *  BranchConfig
 * ══════════════════════════════════════════════════════════════ */

BranchConfig *branch_config_new(const char *resolution, int fps,
                                const char *codec) {
  BranchConfig *branch_cfg = g_new0(BranchConfig, 1);
  g_strlcpy(branch_cfg->resolution, resolution, sizeof(branch_cfg->resolution));
  branch_cfg->fps = fps;
  g_strlcpy(branch_cfg->codec, codec, sizeof(branch_cfg->codec));
  sscanf(resolution, "%dx%d", &branch_cfg->width, &branch_cfg->height);

  /* Initialize with stability-first defaults to prevent 'Zero-Bitrate' edge
   * cases */
  branch_cfg->bitrate = DEFAULT_BRANCH_BITRATE;
  g_strlcpy(branch_cfg->bitrate_mode, DEFAULT_BRANCH_BITRATE_MODE,
            sizeof(branch_cfg->bitrate_mode));

  return branch_cfg;
}

void branch_config_free(BranchConfig *branch_cfg) { g_free(branch_cfg); }

/* ══════════════════════════════════════════════════════════════
 *  Bus Callbacks
 * ══════════════════════════════════════════════════════════════ */

static void _on_bus_error(GstBus *bus, GstMessage *msg, gpointer data) {
  (void)bus;
  (void)data;
  GError *err = NULL;
  gchar *debug = NULL;
  gst_message_parse_error(msg, &err, &debug);
  g_warning("[CAP-BUS] ERROR from '%s': %s  debug: %s",
            GST_OBJECT_NAME(msg->src), err->message, debug ? debug : "none");
  g_error_free(err);
  g_free(debug);
}

static void _on_bus_warning(GstBus *bus, GstMessage *msg, gpointer data) {
  (void)bus;
  (void)data;
  GError *warn = NULL;
  gchar *debug = NULL;
  gst_message_parse_warning(msg, &warn, &debug);
  g_warning("[CAP-BUS] WARNING from '%s': %s  debug: %s",
            GST_OBJECT_NAME(msg->src), warn->message, debug ? debug : "none");
  g_error_free(warn);
  g_free(debug);
}

/* ══════════════════════════════════════════════════════════════
 *  CapturePipeline
 * ══════════════════════════════════════════════════════════════ */

CapturePipeline *capture_pipeline_new(const char *device, const char *caps,
                                      gboolean cairo_capable) {
  CapturePipeline *capture_pipe = g_new0(CapturePipeline, 1);
  g_strlcpy(capture_pipe->device, device, sizeof(capture_pipe->device));
  g_strlcpy(capture_pipe->caps_str, caps, sizeof(capture_pipe->caps_str));
  capture_pipe->dynamic_branches = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)dynamic_branch_free);
  g_mutex_init(&capture_pipe->ai.mutex);
  g_mutex_init(&capture_pipe->branch_mutex);
  capture_pipe->ai.cairo_capable = cairo_capable;
  capture_pipe->ai.ml_w = 640;
  capture_pipe->ai.ml_h = 360;
  return capture_pipe;
}

void capture_pipeline_start(CapturePipeline *capture_pipe) {
  if (capture_pipe->pipeline)
    return;

  g_info(
      "[CAP] Starting capture pipeline: device=%s  caps='%s'  cairo_capable=%s",
      capture_pipe->device, capture_pipe->caps_str,
      capture_pipe->ai.cairo_capable ? "TRUE" : "FALSE");

  const char *dev_short = strrchr(capture_pipe->device, '/');
  dev_short = dev_short ? dev_short + 1 : capture_pipe->device;
  char pipe_name[128];
  snprintf(pipe_name, sizeof(pipe_name), "capture_%s", dev_short);
  capture_pipe->pipeline = gst_pipeline_new(pipe_name);

  GstElement *src;
  if (g_file_test(capture_pipe->device, G_FILE_TEST_EXISTS)) {
    src = gst_element_factory_make("v4l2src", "v4l2src");
    g_object_set(src, "device", capture_pipe->device, NULL);
  } else {
    g_warning("[CAP] Device %s not found — using videotestsrc",
              capture_pipe->device);
    src = gst_element_factory_make("videotestsrc", "v4l2src");
    g_object_set(src, "is-live", TRUE, NULL);
  }

  GstElement *capsfilter = gst_element_factory_make("capsfilter", "src_caps");
  GstCaps *src_caps = gst_caps_from_string(capture_pipe->caps_str);
  g_object_set(capsfilter, "caps", src_caps, NULL);
  gst_caps_unref(src_caps);

  capture_pipe->capture_tee = gst_element_factory_make("tee", "capture_tee");
  g_object_set(capture_pipe->capture_tee, "allow-not-linked", TRUE, NULL);

  gst_bin_add_many(GST_BIN(capture_pipe->pipeline), src, capsfilter,
                   capture_pipe->capture_tee, NULL);
  gst_element_link_many(src, capsfilter, capture_pipe->capture_tee, NULL);

  /* BGRx Global Tee */
  capture_pipe->capture_bgrx_tee =
      gst_element_factory_make("tee", "capture_bgrx_tee");
  g_object_set(capture_pipe->capture_bgrx_tee, "allow-not-linked", TRUE, NULL);

  GstElement *q_g2d = gst_element_factory_make("queue", "bgrx_q_g2d");
  g_object_set(q_g2d, "max-size-buffers", 2, "max-size-time", (guint64)0,
               "leaky", 2, NULL);

  const char *g2n = gst_element_factory_find("imxvideoconvert_g2d")
                        ? "imxvideoconvert_g2d"
                        : "videoconvert";
  GstElement *global_g2d = gst_element_factory_make(g2n, "bgrx_global_g2d");

  int src_width = 1920, src_height = 1080;
  if (sscanf(capture_pipe->caps_str,
             "video/x-raw,format=NV12,width=%d,height=%d", &src_width,
             &src_height) != 2) {
    g_warning("[CAP] Failed to parse resolution from caps string: %s",
              capture_pipe->caps_str);
  }
  capture_pipe->tee_width = src_width;
  capture_pipe->tee_height = src_height;
  g_info("[CAP] BGRx Path initialized for source resolution: %dx%d",
         capture_pipe->tee_width, capture_pipe->tee_height);

  GstElement *global_caps =
      gst_element_factory_make("capsfilter", "bgrx_global_caps");
  char bgrx_caps_str[256];
  snprintf(bgrx_caps_str, sizeof(bgrx_caps_str),
           "video/x-raw,format=BGRx,colorimetry=1:1:16:4,width=%d,height=%d",
           capture_pipe->tee_width, capture_pipe->tee_height);
  GstCaps *sc_bgrx = gst_caps_from_string(bgrx_caps_str);
  g_object_set(global_caps, "caps", sc_bgrx, NULL);
  gst_caps_unref(sc_bgrx);

  GstElement *dummy_q2 = gst_element_factory_make("queue", "dummy_q2");
  GstElement *dummy_sink2 = gst_element_factory_make("fakesink", "dummy_sink2");
  GstElement *dummy_q = gst_element_factory_make("queue", "dummy_q");
  GstElement *dummy_sink = gst_element_factory_make("fakesink", "dummy_sink");
  g_object_set(dummy_q2, "max-size-buffers", 2, "leaky", 2, NULL);
  g_object_set(dummy_q, "max-size-buffers", 2, "leaky", 2, NULL);
  g_object_set(dummy_sink2, "sync", FALSE, NULL);
  g_object_set(dummy_sink, "sync", FALSE, NULL);

  gst_bin_add_many(GST_BIN(capture_pipe->pipeline), q_g2d, global_g2d,
                   global_caps, capture_pipe->capture_bgrx_tee, dummy_q2,
                   dummy_sink2, dummy_q, dummy_sink, NULL);
  gst_element_link_many(q_g2d, global_g2d, global_caps,
                        capture_pipe->capture_bgrx_tee, NULL);

  GstPad *tp_g2d =
      gst_element_request_pad_simple(capture_pipe->capture_tee, "src_%u");
  GstPad *g2d_sink = gst_element_get_static_pad(q_g2d, "sink");
  gst_pad_link(tp_g2d, g2d_sink);
  gst_object_unref(g2d_sink);
  gst_object_unref(tp_g2d);

  GstPad *tp2 =
      gst_element_request_pad_simple(capture_pipe->capture_bgrx_tee, "src_%u");
  GstPad *dq2_sink = gst_element_get_static_pad(dummy_q2, "sink");
  gst_pad_link(tp2, dq2_sink);
  gst_object_unref(dq2_sink);
  gst_object_unref(tp2);
  gst_element_link(dummy_q2, dummy_sink2);

  GstPad *teepad =
      gst_element_request_pad_simple(capture_pipe->capture_tee, "src_%u");
  GstPad *dq_sink = gst_element_get_static_pad(dummy_q, "sink");
  gst_pad_link(teepad, dq_sink);
  gst_object_unref(dq_sink);
  gst_object_unref(teepad);
  gst_element_link(dummy_q, dummy_sink);

  /* Snapshot appsink */
  GstElement *snap_q = gst_element_factory_make("queue", "snap_q");
  g_object_set(snap_q, "max-size-buffers", 1, "leaky", 2, NULL);
  capture_pipe->snap_sink = gst_element_factory_make("appsink", "snap_sink");
  g_object_set(capture_pipe->snap_sink, "emit-signals", FALSE, "drop", TRUE,
               "max-buffers", 1, "sync", FALSE, NULL);
  gst_bin_add_many(GST_BIN(capture_pipe->pipeline), snap_q,
                   capture_pipe->snap_sink, NULL);
  GstPad *snap_pad =
      gst_element_request_pad_simple(capture_pipe->capture_bgrx_tee, "src_%u");
  GstPad *sq_sink = gst_element_get_static_pad(snap_q, "sink");
  gst_pad_link(snap_pad, sq_sink);
  gst_object_unref(sq_sink);
  gst_object_unref(snap_pad);
  gst_element_link(snap_q, capture_pipe->snap_sink);

  /* Bus */
  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(capture_pipe->pipeline));
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message::error", G_CALLBACK(_on_bus_error), NULL);
  g_signal_connect(bus, "message::warning", G_CALLBACK(_on_bus_warning), NULL);
  gst_object_unref(bus);

  gst_element_set_state(capture_pipe->pipeline, GST_STATE_PLAYING);
  GstState state;
  GstStateChangeReturn status = gst_element_get_state(
      capture_pipe->pipeline, &state, NULL, 5 * GST_SECOND);
  if (status != GST_STATE_CHANGE_SUCCESS) {
    g_error("[CAP] Pipeline %s failed to reach PLAYING", capture_pipe->device);
  } else {
    g_info("[CAP] Capture pipeline %s → PLAYING", capture_pipe->device);
    capture_pipeline_dump_dot(capture_pipe, "CAPTURE_START");
    if (capture_pipe->ai.shm_requested)
      capture_pipeline_attach_ai_shm(capture_pipe, capture_pipe->ai.shm_socket);
  }
}

void capture_pipeline_dump_dot(CapturePipeline *capture_pipe,
                               const char *name) {
  if (!capture_pipe->pipeline)
    return;
  const char *dot_dir = g_getenv("GST_DEBUG_DUMP_DOT_DIR");
  if (!dot_dir)
    dot_dir = "/tmp";

  char dev_safe[64];
  g_strlcpy(dev_safe, capture_pipe->device, sizeof(dev_safe));
  for (char *p = dev_safe; *p; p++)
    if (*p == '/')
      *p = '_';

  char dot_name[256];
  snprintf(dot_name, sizeof(dot_name), "pipeline_%s_%s", name, dev_safe);
  GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(capture_pipe->pipeline),
                            GST_DEBUG_GRAPH_SHOW_ALL |
                                GST_DEBUG_GRAPH_SHOW_CAPS_DETAILS |
                                GST_DEBUG_GRAPH_SHOW_VERBOSE,
                            dot_name);
  g_info("[CAP] DOT dump: %s/%s.dot", dot_dir, dot_name);
}

GstSample *capture_pipeline_grab_snapshot(CapturePipeline *capture_pipe) {
  if (!capture_pipe->snap_sink)
    return NULL;
  GstSample *sample = NULL;
  g_signal_emit_by_name(capture_pipe->snap_sink, "pull-sample", &sample);
  if (!sample)
    g_warning("[CAP] grab_snapshot: no sample available");
  return sample;
}

void capture_pipeline_stop(CapturePipeline *capture_pipe) {
  if (!capture_pipe->pipeline)
    return;

  g_info("[CAP] Stopping capture pipeline %s...", capture_pipe->device);
  gst_element_send_event(capture_pipe->pipeline, gst_event_new_eos());

  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(capture_pipe->pipeline));
  GstMessage *msg = gst_bus_timed_pop_filtered(
      bus, 2 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  if (msg) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError *err = NULL;
      gst_message_parse_error(msg, &err, NULL);
      g_warning("[CAP] ERROR during EOS on %s: %s", capture_pipe->device,
                err->message);
      g_error_free(err);
    }
    gst_message_unref(msg);
  }
  gst_object_unref(bus);

  gst_element_set_state(capture_pipe->pipeline, GST_STATE_NULL);
  gst_object_unref(capture_pipe->pipeline);
  capture_pipe->pipeline = NULL;
  capture_pipe->capture_tee = NULL;
  capture_pipe->capture_bgrx_tee = NULL;

  g_info("[CAP] Pipeline %s stopped.", capture_pipe->device);
}

void capture_pipeline_free(CapturePipeline *capture_pipe) {
  if (!capture_pipe)
    return;
  capture_pipeline_stop(capture_pipe);
  g_hash_table_destroy(capture_pipe->dynamic_branches);
  g_mutex_clear(&capture_pipe->branch_mutex);
  g_mutex_clear(&capture_pipe->ai.mutex);
  if (capture_pipe->ai.coords)
    g_free(capture_pipe->ai.coords);
  g_free(capture_pipe);
}

static void dynamic_branch_free(gpointer data) {
  DynamicBranch *active_branch = (DynamicBranch *)data;
  if (!active_branch)
    return;
  g_debug("[CAP-BR] dynamic_branch_free: %s", active_branch->name);
  if (active_branch->elements)
    g_free(active_branch->elements);
  g_free(active_branch);
}

static DynamicBranch *_dynamic_branch_new(const char *name,
                                          CapturePipeline *capture_pipe,
                                          GstElement **branch_elements,
                                          int num_elements,
                                          gboolean use_nv12_tee) {
  DynamicBranch *active_branch = g_new0(DynamicBranch, 1);
  g_strlcpy(active_branch->name, name, sizeof(active_branch->name));
  active_branch->capture = capture_pipe;
  active_branch->num_elements = num_elements;
  active_branch->use_nv12_tee = use_nv12_tee;
  active_branch->elements = g_new(GstElement *, num_elements);
  memcpy(active_branch->elements, branch_elements,
         sizeof(GstElement *) * num_elements);
  return active_branch;
}

static gboolean _dynamic_branch_attach(DynamicBranch *active_branch) {
  CapturePipeline *capture_pipe = active_branch->capture;

  for (int i = 0; i < active_branch->num_elements; i++) {
    if (!gst_bin_add(GST_BIN(capture_pipe->pipeline),
                     active_branch->elements[i])) {
      g_warning("[CAP-BR] gst_bin_add failed for element %d of '%s'", i,
                active_branch->name);
    }
  }

  if (active_branch->num_elements > 1) {
    for (int i = 0; i < active_branch->num_elements - 1; i++) {
      if (!gst_element_link(active_branch->elements[i],
                            active_branch->elements[i + 1])) {
        g_warning("[CAP-BR] Internal link failed at index %d for '%s'", i,
                  active_branch->name);
        goto cleanup_bin;
      }
    }
  }

  active_branch->teepad =
      active_branch->use_nv12_tee
          ? gst_element_request_pad_simple(capture_pipe->capture_tee, "src_%u")
          : gst_element_request_pad_simple(capture_pipe->capture_bgrx_tee,
                                           "src_%u");

  if (!active_branch->teepad) {
    g_warning("[CAP-BR] Tee pad request FAILED for '%s'", active_branch->name);
    goto cleanup_bin;
  }

  for (int i = 0; i < active_branch->num_elements; i++) {
    if (!gst_element_sync_state_with_parent(active_branch->elements[i])) {
      g_warning("[CAP-BR] sync_state_with_parent failed for element %d of '%s'",
                i, active_branch->name);
    }
  }

  {
    GstPad *sinkpad =
        gst_element_get_static_pad(active_branch->elements[0], "sink");
    if (!sinkpad) {
      g_warning("[CAP-BR] First element of '%s' has no static sink pad",
                active_branch->name);
      GstElement *tee_elem = active_branch->use_nv12_tee
                                 ? capture_pipe->capture_tee
                                 : capture_pipe->capture_bgrx_tee;
      gst_element_release_request_pad(tee_elem, active_branch->teepad);
      gst_object_unref(active_branch->teepad);
      active_branch->teepad = NULL;
      goto cleanup_bin;
    }

    GstPadLinkReturn link_ret = gst_pad_link(active_branch->teepad, sinkpad);
    gst_object_unref(sinkpad);

    if (link_ret != GST_PAD_LINK_OK) {
      g_warning("[CAP-BR] gst_pad_link FAILED for '%s': %s (%d)",
                active_branch->name, gst_pad_link_get_name(link_ret), link_ret);
      GstElement *tee_elem = active_branch->use_nv12_tee
                                 ? capture_pipe->capture_tee
                                 : capture_pipe->capture_bgrx_tee;
      gst_element_release_request_pad(tee_elem, active_branch->teepad);
      gst_object_unref(active_branch->teepad);
      active_branch->teepad = NULL;
      goto cleanup_bin;
    }
  }

  g_info("[CAP-BR] ✓ Branch '%s' attached (%d elements)  tee=%s  pad=%s",
         active_branch->name, active_branch->num_elements,
         active_branch->use_nv12_tee ? "NV12/capture_tee"
                                     : "BGRx/capture_bgrx_tee",
         GST_PAD_NAME(active_branch->teepad));
  capture_pipeline_dump_dot(capture_pipe, active_branch->name);
  return TRUE;

cleanup_bin:
  for (int i = active_branch->num_elements - 1; i >= 0; i--) {
    gst_element_set_state(active_branch->elements[i], GST_STATE_NULL);
    gst_bin_remove(GST_BIN(capture_pipe->pipeline), active_branch->elements[i]);
  }
  return FALSE;
}

/* ── Async Teardown Machinery ── */

typedef struct {
  GstElement **elements;
  int num_elements;
  GstElement *pipeline;
  CapturePipeline *capture_pipe;
  DynamicBranch *active_branch;
  GstPad *teepad;
  GstElement *tee_elem;
  char branch_name[64];
  guint timeout_id;
  gint done;
  gint ref_count;
  gboolean is_recording;
} TeardownCtx;

static void _teardown_ctx_unref(gpointer data) {
  TeardownCtx *ctx = (TeardownCtx *)data;
  if (!ctx)
    return;
  if (g_atomic_int_dec_and_test(&ctx->ref_count)) {
    if (ctx->elements)
      g_free(ctx->elements);
    if (ctx->active_branch)
      g_free(ctx->active_branch);
    g_free(ctx);
  }
}

static TeardownCtx *_teardown_ctx_ref(TeardownCtx *ctx) {
  if (ctx)
    g_atomic_int_inc(&ctx->ref_count);
  return ctx;
}

static gboolean _teardown_run(gpointer data) {
  TeardownCtx *ctx = (TeardownCtx *)data;
  if (!ctx)
    return G_SOURCE_REMOVE;

  if (!g_atomic_int_compare_and_exchange(&ctx->done, FALSE, TRUE))
    return G_SOURCE_REMOVE;

  if (ctx->timeout_id > 0) {
    guint tid = ctx->timeout_id;
    ctx->timeout_id = 0;
    g_source_remove(tid);
  }

  g_info("[CAP-TEAR] '%s' — NULLing %d elements", ctx->branch_name,
         ctx->num_elements);

  /* [FIX-STATE] Set all elements to NULL with state verification.
   * After gst_element_get_state() returns, verify the element ACTUALLY
   * reached NULL. If it didn't (stuck encoder/muxer), retry with a longer
   * timeout before proceeding. */
  for (int i = ctx->num_elements - 1; i >= 0; i--) {
    if (ctx->elements[i] && GST_IS_ELEMENT(ctx->elements[i])) {
      g_signal_handlers_disconnect_by_data(ctx->elements[i], ctx->capture_pipe);
      gst_element_set_state(ctx->elements[i], GST_STATE_NULL);

      GstState actual_state = GST_STATE_VOID_PENDING;
      GstStateChangeReturn ret = gst_element_get_state(
          ctx->elements[i], &actual_state, NULL, 2 * GST_SECOND);

      if (actual_state != GST_STATE_NULL) {
        g_warning("[CAP-TEAR] '%s' — element[%d] stuck in %s (ret=%d), "
                  "retrying NULL with 5s timeout",
                  ctx->branch_name, i,
                  gst_element_state_get_name(actual_state), ret);
        gst_element_set_state(ctx->elements[i], GST_STATE_NULL);
        gst_element_get_state(ctx->elements[i], &actual_state, NULL,
                              5 * GST_SECOND);
        if (actual_state != GST_STATE_NULL) {
          g_warning("[CAP-TEAR] '%s' — element[%d] STILL not NULL (%s) "
                    "after retry, proceeding with removal anyway",
                    ctx->branch_name, i,
                    gst_element_state_get_name(actual_state));
        }
      }
    }
  }

  /* [FIX-BIN-REMOVE] Verify element is a child of the bin before removing.
   * This prevents CRITICAL when an element was already removed by another
   * teardown path (e.g., HLS + RTSP racing on the same branch). */
  if (ctx->pipeline && GST_IS_BIN(ctx->pipeline)) {
    for (int i = ctx->num_elements - 1; i >= 0; i--) {
      if (ctx->elements[i] && GST_IS_ELEMENT(ctx->elements[i])) {
        GstObject *parent = gst_element_get_parent(ctx->elements[i]);
        if (parent) {
          gst_bin_remove(GST_BIN(ctx->pipeline), ctx->elements[i]);
          gst_object_unref(parent);
        } else {
          g_debug("[CAP-TEAR] '%s' — element[%d] has no parent, "
                  "skipping gst_bin_remove",
                  ctx->branch_name, i);
        }
      }
    }
  }

  if (ctx->tee_elem && GST_IS_ELEMENT(ctx->tee_elem) && ctx->teepad &&
      GST_IS_PAD(ctx->teepad)) {
    gst_element_release_request_pad(ctx->tee_elem, ctx->teepad);
    g_info("[CAP-TEAR] '%s' — tee pad released", ctx->branch_name);
  }
  if (ctx->teepad) {
    gst_object_unref(ctx->teepad);
    ctx->teepad = NULL;
  }

  g_info("[CAP-TEAR] ✓ Branch '%s' fully removed", ctx->branch_name);
  return G_SOURCE_REMOVE;
}

static GstPadProbeReturn _eos_probe_cb(GstPad *pad, GstPadProbeInfo *info_ptr,
                                       gpointer data) {
  (void)pad;
  TeardownCtx *ctx = (TeardownCtx *)data;
  if (!ctx || g_atomic_int_get(&ctx->done))
    return GST_PAD_PROBE_REMOVE;

  GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info_ptr);
  if (GST_EVENT_TYPE(event) != GST_EVENT_EOS)
    return GST_PAD_PROBE_OK;

  if (ctx->timeout_id > 0) {
    guint tid = ctx->timeout_id;
    ctx->timeout_id = 0;
    g_source_remove(tid);
  }

  if (ctx->is_recording) {
    g_info(
        "[CAP-TEAR] '%s' — EOS reached muxer, waiting 1000ms for finalization",
        ctx->branch_name);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, _teardown_run,
                       _teardown_ctx_ref(ctx), _teardown_ctx_unref);
  } else {
    g_info("[CAP-TEAR] '%s' — EOS reached parser output, "
           "scheduling teardown",
           ctx->branch_name);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, _teardown_run,
                    _teardown_ctx_ref(ctx), _teardown_ctx_unref);
  }

  return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn _unlink_and_cleanup_probe_cb(GstPad *pad,
                                                      GstPadProbeInfo *info_ptr,
                                                      gpointer data) {
  (void)info_ptr;
  DynamicBranch *active_branch = (DynamicBranch *)data;
  if (!active_branch)
    return GST_PAD_PROBE_REMOVE;

  if (!g_atomic_int_compare_and_exchange((gint *)&active_branch->removing, TRUE,
                                         TRUE))
    return GST_PAD_PROBE_OK;

  CapturePipeline *capture_pipe = active_branch->capture;

  g_info("[CAP-TEAR] '%s' — IDLE probe fired, unlinking from tee",
         active_branch->name);

  GstPad *sinkpad =
      gst_element_get_static_pad(active_branch->elements[0], "sink");
  if (sinkpad) {
    gst_pad_unlink(pad, sinkpad);
  }

  TeardownCtx *ctx = g_new0(TeardownCtx, 1);
  ctx->elements = active_branch->elements;
  ctx->num_elements = active_branch->num_elements;
  ctx->pipeline = capture_pipe->pipeline;
  ctx->capture_pipe = capture_pipe;
  ctx->active_branch = active_branch;
  ctx->teepad = pad;
  ctx->tee_elem = active_branch->use_nv12_tee ? capture_pipe->capture_tee
                                              : capture_pipe->capture_bgrx_tee;
  ctx->done = FALSE;
  ctx->ref_count = 1;
  ctx->is_recording = g_str_has_prefix(active_branch->name, "rec_");
  g_strlcpy(ctx->branch_name, active_branch->name, sizeof(ctx->branch_name));

  active_branch->elements = NULL;
  active_branch->teepad = NULL;

  GstPad *eos_detect_pad = NULL;

  /* [FIX-EOS-DETECT] Monitor EOS near the END of the chain, but NOT on
   * the very last element (appsink) for non-recording branches.
   *
   * WHY NOT the appsink?
   *   stream_manager_release() sets the appsink to GST_STATE_NULL *before*
   *   calling capture_pipeline_remove_branch(). A NULLed element's sink pad
   *   is in flushing state — downstream events (including EOS) pushed to a
   *   flushing pad are silently DROPPED before any pad probe can fire.
   *   Same race exists with stream_manager_force_teardown().
   *
   * WHY the second-to-last element's SRC pad?
   *   The parser (h264parse/h265parse) is the second-to-last element and
   *   remains in PLAYING state throughout teardown. Placing the probe on
   *   its src pad means the probe fires when the parser PUSHES EOS
   *   downstream — at that point the entire encoding chain has fully
   *   processed EOS, and it's safe to NULL + remove all elements.
   *
   *   Chain: queue → scaler → caps → rate → enc → parser → appsink
   *   Probe here ────────────────────────────────────┘
   *
   *   Original bug: EOS probe was on elements[0]->sink (injection pad),
   *   firing instantly before EOS propagated through the chain.
   *
   * The 10-second timeout remains as fallback if EOS gets stuck. */
  if (ctx->is_recording) {
    /* Recording: detect EOS at the muxer's sink (second-to-last → last) */
    if (ctx->num_elements > 1) {
      GstPad *prev_src = gst_element_get_static_pad(
          ctx->elements[ctx->num_elements - 2], "src");
      if (prev_src) {
        eos_detect_pad = gst_pad_get_peer(prev_src);
        gst_object_unref(prev_src);
      }
    }
    if (!eos_detect_pad) {
      eos_detect_pad = gst_element_get_static_pad(
          ctx->elements[ctx->num_elements - 1], "sink");
    }
  } else {
    /* Non-recording (RTSP/HLS encoder branches):
     * Use the second-to-last element's SRC pad (parser output).
     * The appsink may be NULLed → its sink pad is flushing → probe
     * would never fire. Parser is always PLAYING during teardown. */
    if (ctx->num_elements > 1) {
      eos_detect_pad = gst_element_get_static_pad(
          ctx->elements[ctx->num_elements - 2], "src");
      g_info("[CAP-TEAR] '%s' — EOS probe on parser src pad "
             "(avoids NULLed appsink race)",
             ctx->branch_name);
    }
    if (!eos_detect_pad) {
      /* Fallback: single-element branch or pad not found */
      eos_detect_pad = gst_element_get_static_pad(
          ctx->elements[ctx->num_elements - 1], "sink");
    }
  }

  if (eos_detect_pad) {
    gst_pad_add_probe(eos_detect_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      _eos_probe_cb, _teardown_ctx_ref(ctx),
                      _teardown_ctx_unref);
    gst_object_unref(eos_detect_pad);
  }

  if (sinkpad) {
    gst_pad_send_event(sinkpad, gst_event_new_eos());
    g_info("[CAP-TEAR] '%s' — EOS injected at branch start", ctx->branch_name);
    gst_object_unref(sinkpad);
  }

  ctx->timeout_id =
      g_timeout_add_full(G_PRIORITY_DEFAULT, 10000, _teardown_run,
                         _teardown_ctx_ref(ctx), _teardown_ctx_unref);

  _teardown_ctx_unref(ctx);
  return GST_PAD_PROBE_REMOVE;
}

/* ── Public API ── */

DynamicBranch *capture_pipeline_add_branch(CapturePipeline *capture_pipe,
                                           const char *name,
                                           GstElement **branch_elements,
                                           int num_elements,
                                           gboolean use_nv12_tee) {
  g_mutex_lock(&(capture_pipe->branch_mutex));
  if (g_hash_table_contains(capture_pipe->dynamic_branches, name)) {
    g_warning("[CAP-BR] add_branch: branch '%s' already exists", name);
    g_mutex_unlock(&(capture_pipe->branch_mutex));
    return NULL;
  }

  for (int i = 0; i < num_elements; i++) {
    if (!branch_elements[i])
      continue;
    GstElementFactory *f = gst_element_get_factory(branch_elements[i]);
    if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)),
                       "cairooverlay") == 0) {
      if (capture_pipe->ai.cairo_capable)
        g_signal_connect(branch_elements[i], "draw", G_CALLBACK(_on_cairo_draw),
                         capture_pipe);
    }
  }

  DynamicBranch *active_branch = _dynamic_branch_new(
      name, capture_pipe, branch_elements, num_elements, use_nv12_tee);
  if (!_dynamic_branch_attach(active_branch)) {
    g_warning("[CAP-BR] Branch '%s' FAILED to attach", name);
    g_free(active_branch->elements);
    g_free(active_branch);
    g_mutex_unlock(&(capture_pipe->branch_mutex));
    return NULL;
  }

  g_hash_table_insert(capture_pipe->dynamic_branches, g_strdup(name),
                      active_branch);
  g_info("[CAP-BR] ✓ Branch '%s' added  active=%u", name,
         g_hash_table_size(capture_pipe->dynamic_branches));
  g_mutex_unlock(&(capture_pipe->branch_mutex));
  return active_branch;
}

void capture_pipeline_remove_branch(CapturePipeline *capture_pipe,
                                    const char *name) {
  if (!capture_pipe || !name)
    return;

  g_mutex_lock(&(capture_pipe->branch_mutex));
  DynamicBranch *active_branch =
      g_hash_table_lookup(capture_pipe->dynamic_branches, name);
  if (!active_branch) {
    g_debug("[CAP-BR] remove_branch: '%s' not found", name);
    g_mutex_unlock(&(capture_pipe->branch_mutex));
    return;
  }
  if (active_branch->removing) {
    g_debug("[CAP-BR] remove_branch: '%s' already being removed", name);
    g_mutex_unlock(&(capture_pipe->branch_mutex));
    return;
  }
  active_branch->removing = TRUE;

  g_hash_table_steal(capture_pipe->dynamic_branches, name);
  g_mutex_unlock(&(capture_pipe->branch_mutex));

  g_info("[CAP-BR] Scheduling IDLE probe to remove branch '%s'", name);

  if (!active_branch->teepad) {
    if (active_branch->elements) {
      for (int i = active_branch->num_elements - 1; i >= 0; i--) {
        gst_element_set_state(active_branch->elements[i], GST_STATE_NULL);
        if (capture_pipe->pipeline)
          gst_bin_remove(GST_BIN(capture_pipe->pipeline),
                         active_branch->elements[i]);
      }
      g_free(active_branch->elements);
    }
    g_free(active_branch);
    return;
  }

  gst_pad_add_probe(active_branch->teepad, GST_PAD_PROBE_TYPE_IDLE,
                    _unlink_and_cleanup_probe_cb, active_branch, NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  AI SHM Attach/Detach
 * ══════════════════════════════════════════════════════════════ */

void capture_pipeline_attach_ai_shm(CapturePipeline *capture_pipe,
                                    const char *socket_path) {
  if (!capture_pipe || !socket_path)
    return;
  if (!capture_pipe->pipeline) {
    capture_pipe->ai.shm_requested = TRUE;
    g_strlcpy(capture_pipe->ai.shm_socket, socket_path,
              sizeof(capture_pipe->ai.shm_socket));
    g_info("[CAP-SHM] Pipeline not ready — SHM attach deferred for %s",
           capture_pipe->device);
    return;
  }

  capture_pipe->ai.shm_requested = TRUE;
  g_strlcpy(capture_pipe->ai.shm_socket, socket_path,
            sizeof(capture_pipe->ai.shm_socket));

  if (g_hash_table_contains(capture_pipe->dynamic_branches, "ai_shm"))
    capture_pipeline_remove_branch(capture_pipe, "ai_shm");

  GstElement *q = gst_element_factory_make("queue", "ai_shm_q");
  g_object_set(q, "max-size-buffers", 2, "leaky", 2, NULL);

  GstElement *rate = gst_element_factory_make("videorate", "ai_shm_rate");
  g_object_set(rate, "max-rate", 10, "drop-only", TRUE, NULL);

  GstElement *sink = gst_element_factory_make("shmsink", "ai_shmsink");
  if (!sink) {
    g_warning("[CAP-SHM] shmsink not available");
    gst_object_unref(q);
    gst_object_unref(rate);
    return;
  }
  g_object_set(sink, "socket-path", socket_path, "shm-size", 67108864,
               "wait-for-connection", FALSE, "sync", FALSE, NULL);

  GstElement *elements[] = {q, rate, sink};
  if (capture_pipeline_add_branch(capture_pipe, "ai_shm", elements, 3, TRUE))
    g_info("[CAP-SHM] ✓ AI SHM attached: %s → %s", capture_pipe->device,
           socket_path);
  else
    g_warning("[CAP-SHM] AI SHM FAILED to attach to %s", capture_pipe->device);
}

void capture_pipeline_detach_ai_shm(CapturePipeline *capture_pipe) {
  if (!capture_pipe)
    return;
  if (!g_hash_table_contains(capture_pipe->dynamic_branches, "ai_shm")) {
    g_debug("[CAP-SHM] detach_ai_shm: no AI SHM branch for %s",
            capture_pipe->device);
    return;
  }
  g_info("[CAP-SHM] Detaching AI SHM from %s...", capture_pipe->device);
  capture_pipe->ai.shm_requested = FALSE;
  capture_pipeline_remove_branch(capture_pipe, "ai_shm");
}
