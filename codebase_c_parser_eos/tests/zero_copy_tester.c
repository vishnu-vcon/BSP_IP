#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>

#define MAX_LENSES 2
#define MAX_COORDS 32
#define MAX_SUBSCRIBERS 2

typedef struct {
    float left, top, right, bottom, conf;
    char label[32];
} AICoord;

typedef struct {
    GMutex mutex;
    AICoord coords[MAX_COORDS];
    int num_coords;
} AILensState;

typedef struct {
    char device[64];
    GstElement *pipeline;
    GstElement *capture_tee;
    GstElement *capture_bgrx_tee;
    AILensState ai;
    int id;
} CapturePipeline;

typedef struct {
    CapturePipeline *cap;
    GstElement *appsink;
    char name[64];
} ManagedStream;

typedef struct {
    GMainLoop *loop;
    CapturePipeline *lenses[MAX_LENSES];
    int num_active_lenses;
    gboolean running;
} AppCtx;

static void log_cpu_usage(const char *tag) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    printf("[STATS] %-10s | UserCPU: %ld.%03lds total\n", tag, usage.ru_utime.tv_sec, usage.ru_utime.tv_usec / 1000);
}

static void on_cairo_draw(GstElement *overlay, cairo_t *cr, guint64 timestamp, guint64 duration, gpointer data) {
    CapturePipeline *cap = (CapturePipeline *)data;
    if (!cap) return;
    
    g_mutex_lock(&cap->ai.mutex);
    int n = cap->ai.num_coords;
    cairo_set_line_width(cr, 3.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 24.0);

    for (int i = 0; i < n; i++) {
        AICoord *c = &cap->ai.coords[i];
        cairo_set_source_rgb(cr, 0.0, 1.0, 0.0);
        cairo_rectangle(cr, c->left * 1920, c->top * 1080, (c->right-c->left)*1920, (c->bottom-c->top)*1080);
        cairo_stroke(cr);
    }
    g_mutex_unlock(&cap->ai.mutex);
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer data) {
    ManagedStream *ms = (ManagedStream *)data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (buf) {
        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
            GstBuffer *copy = gst_buffer_copy_region(
                buf, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META,
                0, gst_buffer_get_size(buf));
            if (copy) gst_buffer_unref(copy);
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void *ai_simulation_thread(void *arg) {
    AppCtx *ctx = (AppCtx *)arg;
    while (ctx->running) {
        for (int i = 0; i < ctx->num_active_lenses; i++) {
            CapturePipeline *cap = ctx->lenses[i];
            if (!cap) continue;
            g_mutex_lock(&cap->ai.mutex);
            cap->ai.num_coords = 5;
            for (int j = 0; j < 5; j++) {
                cap->ai.coords[j].left = 0.1 * j; cap->ai.coords[j].top = 0.1 * j;
                cap->ai.coords[j].right = 0.2 * j; cap->ai.coords[j].bottom = 0.2 * j;
            }
            g_mutex_unlock(&cap->ai.mutex);
        }
        usleep(100000);
    }
    return NULL;
}

#define CHECK_ELEM(e, name) if (!(e)) { printf("[ERROR] Failed to create element '%s'\n", name); return NULL; }

static CapturePipeline *create_capture_pipeline(GstElement *pipe, const char *device, int io_mode, int id) {
    CapturePipeline *cap = g_new0(CapturePipeline, 1);
    g_strlcpy(cap->device, device, sizeof(cap->device));
    g_mutex_init(&cap->ai.mutex);
    cap->id = id;

    printf("[STEP] Lens %d: Starting %s init...\n", id, device);

    /* 1. Capture Block */
    GstElement *src = gst_element_factory_make("v4l2src", NULL);
    CHECK_ELEM(src, "v4l2src");
    g_object_set(src, "device", device, "io-mode", io_mode, "num-buffers", 10, NULL);

    GstElement *caps = gst_element_factory_make("capsfilter", NULL);
    CHECK_ELEM(caps, "capsfilter_nv12");
    GstCaps *c1 = gst_caps_from_string("video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1");
    g_object_set(caps, "caps", c1, NULL); gst_caps_unref(c1);

    cap->capture_tee = gst_element_factory_make("tee", NULL);
    CHECK_ELEM(cap->capture_tee, "tee_nv12");
    g_object_set(cap->capture_tee, "allow-not-linked", TRUE, NULL);

    gst_bin_add_many(GST_BIN(pipe), src, caps, cap->capture_tee, NULL);
    if (!gst_element_link_many(src, caps, cap->capture_tee, NULL)) { printf("[ERROR] Link src->tee failed\n"); return NULL; }

    /* 2. BGRx / G2D Block */
    GstElement *q_g2d = gst_element_factory_make("queue", NULL);
    CHECK_ELEM(q_g2d, "q_g2d");
    g_object_set(q_g2d, "max-size-buffers", 2, "leaky", 2, NULL);

    GstElementFactory *f_g2d = gst_element_factory_find("imxvideoconvert_g2d");
    GstElement *g2d = gst_element_factory_create(f_g2d, NULL);
    if (!g2d) g2d = gst_element_factory_make("videoconvert", NULL);
    CHECK_ELEM(g2d, "scaler");

    GstElement *bc = gst_element_factory_make("capsfilter", NULL);
    GstCaps *c2 = gst_caps_from_string("video/x-raw,format=BGRx");
    g_object_set(bc, "caps", c2, NULL); gst_caps_unref(c2);

    cap->capture_bgrx_tee = gst_element_factory_make("tee", NULL);
    g_object_set(cap->capture_bgrx_tee, "allow-not-linked", TRUE, NULL);

    gst_bin_add_many(GST_BIN(pipe), q_g2d, g2d, bc, cap->capture_bgrx_tee, NULL);
    if (!gst_element_link_many(q_g2d, g2d, bc, cap->capture_bgrx_tee, NULL)) { printf("[ERROR] Link g2d block failed\n"); return NULL; }

    GstPad *tp_g = gst_element_request_pad_simple(cap->capture_tee, "src_%u");
    GstPad *sp_g = gst_element_get_static_pad(q_g2d, "sink");
    if (!tp_g || !sp_g || gst_pad_link(tp_g, sp_g) != GST_PAD_LINK_OK) { printf("[ERROR] Link tee->g2d failed\n"); return NULL; }
    if (tp_g) gst_object_unref(tp_g); if (sp_g) gst_object_unref(sp_g);

    /* 3. Encoder / AppSink Block */
    GstElement *q_enc = gst_element_factory_make("queue", NULL);
    GstElement *ovl = gst_element_factory_make("cairooverlay", NULL);
    CHECK_ELEM(ovl, "cairooverlay");
    g_signal_connect(ovl, "draw", G_CALLBACK(on_cairo_draw), cap);

    GstElementFactory *f_enc = gst_element_factory_find("v4l2h264enc");
    GstElement *enc = gst_element_factory_create(f_enc, NULL);
    if (!enc) enc = gst_element_factory_make("x264enc", NULL);
    CHECK_ELEM(enc, "encoder");

    GstElement *as = gst_element_factory_make("appsink", NULL);
    g_object_set(as, "emit-signals", TRUE, "sync", FALSE, NULL);

    ManagedStream *ms = g_new0(ManagedStream, 1);
    ms->cap = cap; ms->appsink = as;
    GstAppSinkCallbacks cb = {0}; cb.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(as), &cb, ms, NULL);

    gst_bin_add_many(GST_BIN(pipe), q_enc, ovl, enc, as, NULL);
    gst_element_link_many(q_enc, ovl, enc, as, NULL);

    GstPad *tp_e = gst_element_request_pad_simple(cap->capture_bgrx_tee, "src_%u");
    GstPad *sp_e = gst_element_get_static_pad(q_enc, "sink");
    if (!tp_e || !sp_e || gst_pad_link(tp_e, sp_e) != GST_PAD_LINK_OK) { printf("[ERROR] Link bgrx_tee->enc failed\n"); return NULL; }
    if (tp_e) gst_object_unref(tp_e); if (sp_e) gst_object_unref(sp_e);

    printf("[STEP] Lens %d: Init complete.\n", id);
    return cap;
}

int main(int argc, char *argv[]) {
    printf("[STEP] Starting Main Program...\n");
    if (argc < 3) { printf("Usage: %s <devs_csv> <io_mode>\n", argv[0]); return 1; }

    printf("[STEP] Initializing GStreamer...\n");
    gst_init(&argc, &argv);
    printf("[STEP] GStreamer Init done.\n");

    AppCtx ctx = {0}; ctx.running = TRUE;
    GstElement *pipeline = gst_pipeline_new("fidelity-tester");

    char *devs = g_strdup(argv[1]);
    int mode = atoi(argv[2]);
    printf("[STEP] Configuring devices: %s with mode %d\n", devs, mode);

    char *tok = strtok(devs, ",");
    while (tok && ctx.num_active_lenses < MAX_LENSES) {
        CapturePipeline *cp = create_capture_pipeline(pipeline, tok, mode, ctx.num_active_lenses);
        if (cp) {
            ctx.lenses[ctx.num_active_lenses] = cp;
            ctx.num_active_lenses++;
        }
        tok = strtok(NULL, ",");
    }

    if (ctx.num_active_lenses == 0) { printf("[FATAL] No valid pipelines created.\n"); return 1; }

    printf("[STEP] Starting AI simulation thread...\n");
    pthread_t ai_thread;
    pthread_create(&ai_thread, NULL, ai_simulation_thread, &ctx);

    printf("[STEP] Setting Pipeline to PLAYING...\n");
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) { printf("[FATAL] Master Pipeline failed to start.\n"); return 1; }

    printf("[STEP] Running profile for 20 seconds...\n");
    for (int i = 0; i < 10; i++) {
        sleep(2);
        log_cpu_usage(mode == 4 ? "OPTIMIZED" : "BASELINE");
    }

    printf("[STEP] Cleaning up...\n");
    ctx.running = FALSE;
    pthread_join(ai_thread, NULL);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_free(devs);
    printf("[DONE]\n");
    return 0;
}
