#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define DEFAULT_DEVICE  "/dev/video3"
#define SRC_W           1920
#define SRC_H           1080
#define SRC_FPS         30
#define DOT_DIR         "/tmp"
#define RTSP_PORT       "8554"
#define RTSP_MOUNT      "/test"

typedef struct {
    GstElement  *pipeline;
    GstElement  *tee;
    GMainLoop   *loop;
    GstElement  *disp_q;
    GstElement  *disp_sink;
    GstElement  *rtsp_q;
    GstElement  *rtsp_scale;
    GstElement  *rtsp_res_caps;
    GstElement  *rtsp_rate;
    GstElement  *rtsp_fps_caps;
    GstElement  *rtsp_enc;
    GstElement  *rtsp_parse;
    GstElement  *rtsp_sink;
    GstPad      *rtsp_teepad;
    gboolean     rtsp_attached;
    int          rtsp_frame_count;
    GstRTSPServer *rtsp_server;
    GstElement    *rtsp_appsrc;
    guint64        rtsp_base_pts;
    gboolean       rtsp_base_set;
    gboolean     rec_attached;
    int          rec_count;
    int          rec_frame_count;
    GstPad      *rec_teepad;
} AppCtx;

static AppCtx ctx = {0};

static void dump_dot(GstElement *bin, const char *tag) {
    char name[256];
    snprintf(name, sizeof(name), "test_%s", tag);
    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(bin), GST_DEBUG_GRAPH_SHOW_ALL, name);
    g_print("DOT -> %s/%s.dot\n", DOT_DIR, name);
}

static gboolean dot_dump_cb(gpointer d) {
    (void)d;
    static int tick = 0;
    tick += 10;
    char name[128];
    snprintf(name, sizeof(name), "test_final_tick_%03d", tick);
    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(ctx.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, name);
    g_print("DOT -> %s/%s.dot\n", DOT_DIR, name);
    return G_SOURCE_CONTINUE;
}

static gboolean on_bus_msg(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL; gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        if(dbg) g_printerr("Debug: %s\n", dbg);
        g_error_free(err); g_free(dbg);
        break;
    }
    default: break;
    }
    return TRUE;
}

static void build_capture_pipeline(const char *device) {
    ctx.pipeline = gst_pipeline_new("main_pipeline");
    GstElement *src = gst_element_factory_make(device ? "v4l2src" : "videotestsrc", "src");
    if (device) g_object_set(src, "device", device, NULL);
    else g_object_set(src, "is-live", TRUE, NULL);

    GstElement *src_caps = gst_element_factory_make("capsfilter", "src_caps");
    char c1[256]; snprintf(c1, sizeof(c1), "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1", SRC_W, SRC_H, SRC_FPS);
    GstCaps *sc1 = gst_caps_from_string(c1); g_object_set(src_caps, "caps", sc1, NULL); gst_caps_unref(sc1);

    const char *g2d_name = gst_element_factory_find("imxvideoconvert_g2d") ? "imxvideoconvert_g2d" : "videoconvert";
    GstElement *g2d = gst_element_factory_make(g2d_name, "cap_g2d");

    GstElement *g2d_caps = gst_element_factory_make("capsfilter", "cap_g2d_caps");
    char c2[256]; snprintf(c2, sizeof(c2), "video/x-raw,format=BGRx,width=%d,height=%d,colorimetry=1:1:16:4", SRC_W, SRC_H);
    GstCaps *sc2 = gst_caps_from_string(c2); g_object_set(g2d_caps, "caps", sc2, NULL); gst_caps_unref(sc2);

    ctx.tee = gst_element_factory_make("tee", "cap_tee");
    g_object_set(ctx.tee, "allow-not-linked", TRUE, NULL);

    GstElement *dummy_q = gst_element_factory_make("queue", "dummy_q");
    g_object_set(dummy_q, "max-size-buffers", 2, "leaky", 2, NULL);
    GstElement *dummy_sink = gst_element_factory_make("fakesink", "dummy_sink");
    g_object_set(dummy_sink, "sync", FALSE, NULL);

    ctx.disp_q = gst_element_factory_make("queue", "disp_q");
    g_object_set(ctx.disp_q, "max-size-buffers", 2, "leaky", 2, NULL);
    ctx.disp_sink = gst_element_factory_make("waylandsink", "disp_sink");
    if (!ctx.disp_sink) ctx.disp_sink = gst_element_factory_make("fakesink", "disp_sink");
    g_object_set(ctx.disp_sink, "sync", FALSE, NULL);

    gst_bin_add_many(GST_BIN(ctx.pipeline), src, src_caps, g2d, g2d_caps, ctx.tee, dummy_q, dummy_sink, ctx.disp_q, ctx.disp_sink, NULL);
    gst_element_link_many(src, src_caps, g2d, g2d_caps, ctx.tee, NULL);

    GstPad *tp1 = gst_element_request_pad_simple(ctx.tee, "src_%u");
    GstPad *qp1 = gst_element_get_static_pad(dummy_q, "sink");
    gst_pad_link(tp1, qp1); gst_object_unref(qp1); gst_object_unref(tp1);
    gst_element_link(dummy_q, dummy_sink);

    GstPad *tp2 = gst_element_request_pad_simple(ctx.tee, "src_%u");
    GstPad *qp2 = gst_element_get_static_pad(ctx.disp_q, "sink");
    gst_pad_link(tp2, qp2); gst_object_unref(qp2); gst_object_unref(tp2);
    gst_element_link(ctx.disp_q, ctx.disp_sink);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(ctx.pipeline));
    gst_bus_add_watch(bus, on_bus_msg, NULL);
    gst_object_unref(bus);
}

static GstFlowReturn on_rtsp_new_sample(GstAppSink *sink, gpointer data) {
    (void)data;
    if (!ctx.rtsp_appsrc) return GST_FLOW_OK;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (!buf) { gst_sample_unref(sample); return GST_FLOW_OK; }

    guint64 pts = GST_BUFFER_PTS(buf);
    if (!ctx.rtsp_base_set) { ctx.rtsp_base_pts = pts; ctx.rtsp_base_set = TRUE; }
    GstBuffer *nb = gst_buffer_copy(buf);
    GST_BUFFER_PTS(nb) = pts - ctx.rtsp_base_pts;
    GST_BUFFER_DTS(nb) = GST_CLOCK_TIME_NONE;
    gst_app_src_push_buffer(GST_APP_SRC(ctx.rtsp_appsrc), nb);
    ctx.rtsp_frame_count++;
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void on_media_configure(GstRTSPMediaFactory *f, GstRTSPMedia *m, gpointer d) {
    (void)f; (void)d;
    GstElement *el = gst_rtsp_media_get_element(m);
    ctx.rtsp_appsrc = gst_bin_get_by_name(GST_BIN(el), "pay_src");
    gst_object_unref(el);
    ctx.rtsp_base_set = FALSE; ctx.rtsp_frame_count = 0;
}
static void on_media_unprepared(GstRTSPMedia *m, gpointer d) { 
    (void)m; (void)d;
    ctx.rtsp_appsrc = NULL; ctx.rtsp_base_set = FALSE; 
}
static void on_media_constructed(GstRTSPMediaFactory *f, GstRTSPMedia *m, gpointer d) {
    (void)f;
    g_signal_connect(m, "unprepared", G_CALLBACK(on_media_unprepared), d);
}

static void start_rtsp_server(void) {
    ctx.rtsp_server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(ctx.rtsp_server, RTSP_PORT);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_media_factory_set_launch(factory,
        "( appsrc name=pay_src is-live=true format=time block=false caps=\"video/x-h264,stream-format=byte-stream,alignment=au\" "
        "! rtph264pay name=pay0 pt=96 config-interval=1 )");
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), NULL);
    g_signal_connect(factory, "media-constructed", G_CALLBACK(on_media_constructed), NULL);
    gst_rtsp_mount_points_add_factory(gst_rtsp_server_get_mount_points(ctx.rtsp_server), RTSP_MOUNT, factory);
    gst_rtsp_server_attach(ctx.rtsp_server, NULL);
}

static void add_rtsp_branch(void) {
    if (ctx.rtsp_attached) return;
    int fps = 25;
    ctx.rtsp_q = gst_element_factory_make("queue", "rtsp_q");
    g_object_set(ctx.rtsp_q, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", (guint64)1000000000, "leaky", 2, NULL);
    ctx.rtsp_rate = gst_element_factory_make("videorate", "rtsp_rate");
    g_object_set(ctx.rtsp_rate, "drop-only", TRUE, "max-rate", fps, "skip-to-first", TRUE, NULL);
    
    /* Extra caps filter for branch */
    GstElement *fps_caps = gst_element_factory_make("capsfilter", "rtsp_fps_caps");
    char fc[128]; snprintf(fc, sizeof(fc), "video/x-raw,framerate=%d/1", fps);
    GstCaps *rc2 = gst_caps_from_string(fc); g_object_set(fps_caps, "caps", rc2, NULL); gst_caps_unref(rc2);

    ctx.rtsp_enc = gst_element_factory_make("v4l2h264enc", "rtsp_enc");
    if(!ctx.rtsp_enc) ctx.rtsp_enc = gst_element_factory_make("x264enc", "rtsp_enc");
    
    ctx.rtsp_parse = gst_element_factory_make("h264parse", "rtsp_parse");
    ctx.rtsp_sink = gst_element_factory_make("appsink", "rtsp_sink");
    g_object_set(ctx.rtsp_sink, "sync", FALSE, "max-buffers", 2, "drop", TRUE, "emit-signals", TRUE, NULL);
    GstAppSinkCallbacks cb = {0}; cb.new_sample = on_rtsp_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(ctx.rtsp_sink), &cb, NULL, NULL);

    gst_bin_add_many(GST_BIN(ctx.pipeline), ctx.rtsp_q, ctx.rtsp_rate, fps_caps, ctx.rtsp_enc, ctx.rtsp_parse, ctx.rtsp_sink, NULL);
    gst_element_link_many(ctx.rtsp_q, ctx.rtsp_rate, fps_caps, ctx.rtsp_enc, ctx.rtsp_parse, ctx.rtsp_sink, NULL);
    
    GstElement *ee[] = {ctx.rtsp_q, ctx.rtsp_rate, fps_caps, ctx.rtsp_enc, ctx.rtsp_parse, ctx.rtsp_sink};
    for(int i=0; i<6; i++) gst_element_sync_state_with_parent(ee[i]);

    ctx.rtsp_teepad = gst_element_request_pad_simple(ctx.tee, "src_%u");
    GstPad *sp = gst_element_get_static_pad(ctx.rtsp_q, "sink");
    gst_pad_link(ctx.rtsp_teepad, sp); gst_object_unref(sp);
    ctx.rtsp_attached = TRUE;
}

static GstElement *rec_elems[16];
static int rec_n_elems = 0;

static void add_rec_branch(const char *p) {
    if (ctx.rec_attached) return;
    int fps = SRC_FPS, w = SRC_W, h = SRC_H, br = 4000000;
    char codec[16] = "h264";
    if (p && *p) {
        char buf[512]; strncpy(buf, p, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
        char *tok = strtok(buf, " ,");
        while(tok) {
            if (strncmp(tok, "fps=", 4)==0) fps = atoi(tok+4);
            if (strncmp(tok, "w=", 2)==0) w = atoi(tok+2);
            if (strncmp(tok, "h=", 2)==0) h = atoi(tok+2);
            if (strncmp(tok, "bitrate=", 8)==0) br = atoi(tok+8);
            if (strncmp(tok, "codec=", 6)==0) strncpy(codec, tok+6, sizeof(codec)-1);
            tok = strtok(NULL, " ,");
        }
    }
    ctx.rec_count++;
    char uid[16]; snprintf(uid, sizeof(uid), "rec%d", ctx.rec_count);
    char path[128]; snprintf(path, sizeof(path), "/tmp/test_%s_%%05d.mp4", uid);
    g_print("Adding Recording: BGRx->G2D->BGRx->Queue->Encoder %dx%d @ %dfps | codec=%s\n", w, h, fps, codec);

    rec_n_elems = 0;

    const char *g2n = gst_element_factory_find("imxvideoconvert_g2d") ? "imxvideoconvert_g2d" : "videoconvert";
    GstElement *scale = gst_element_factory_make(g2n, "rscale");
    rec_elems[rec_n_elems++] = scale;
    
    GstElement *rcaps = gst_element_factory_make("capsfilter", "rrcaps");
    char c2[256]; snprintf(c2, sizeof(c2), "video/x-raw,format=BGRx,width=%d,height=%d,colorimetry=1:1:16:4", w, h);
    GstCaps *caps2 = gst_caps_from_string(c2); g_object_set(rcaps, "caps", caps2, NULL); gst_caps_unref(caps2);
    rec_elems[rec_n_elems++] = rcaps;

    GstElement *q = gst_element_factory_make("queue", "rq");
    g_object_set(q, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", (guint64)1000000000ULL, "leaky", 2, NULL);
    rec_elems[rec_n_elems++] = q;
    
    GstElement *rate = gst_element_factory_make("videorate", "rrate");
    g_object_set(rate, "max-rate", fps, "drop-only", TRUE, "skip-to-first", TRUE, NULL);
    rec_elems[rec_n_elems++] = rate;
    
    const char *en = (g_strcmp0(codec, "h265") == 0) ? "v4l2h265enc" : "v4l2h264enc";
    GstElement *enc = gst_element_factory_make(en, "renc");
    if (!enc && g_strcmp0(codec, "h264")==0) enc = gst_element_factory_make("x264enc", "renc");
    if (enc) {
        GstElementFactory *f = gst_element_get_factory(enc);
        if(f && strstr(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)), "v4l2")) {
           GstStructure *ec = gst_structure_new("controls", "video_bitrate", G_TYPE_INT, br, "video_gop_size", G_TYPE_INT, fps?fps:30, NULL);
           g_object_set(enc, "extra-controls", ec, NULL); gst_structure_free(ec);
        }
    }
    rec_elems[rec_n_elems++] = enc;
    
    GstElement *ident = gst_element_factory_make("identity", "rident");
    g_object_set(ident, "drop-allocation", TRUE, NULL);
    rec_elems[rec_n_elems++] = ident;
    
    GstElement *parse = gst_element_factory_make((g_strcmp0(codec, "h265") == 0) ? "h265parse" : "h264parse", "rparse");
    rec_elems[rec_n_elems++] = parse;
    
    GstElement *q2 = gst_element_factory_make("queue", "rq2");
    g_object_set(q2, "max-size-time", (guint64)30000000000ULL, "max-size-bytes", 100000000, "max-size-buffers", 0, NULL);
    rec_elems[rec_n_elems++] = q2;
    
    GstElement *mux = gst_element_factory_make("splitmuxsink", "rmux");
    g_object_set(mux, "location", path, "max-size-time", (guint64)60*GST_SECOND, 
                 "async-finalize", TRUE, "async-handling", TRUE, 
                 "send-keyframe-requests", FALSE, NULL);
    rec_elems[rec_n_elems++] = mux;
    
    for (int i=0; i < rec_n_elems; i++) gst_bin_add(GST_BIN(ctx.pipeline), rec_elems[i]);
    for (int i=0; i < rec_n_elems - 1; i++) gst_element_link(rec_elems[i], rec_elems[i+1]);
    for (int i=0; i < rec_n_elems; i++) gst_element_sync_state_with_parent(rec_elems[i]);
    
    ctx.rec_frame_count = 0;
    
    ctx.rec_teepad = gst_element_request_pad_simple(ctx.tee, "src_%u");
    GstPad *sp = gst_element_get_static_pad(scale, "sink");
    if(gst_pad_link(ctx.rec_teepad, sp) != GST_PAD_LINK_OK) g_print("Failed to link rec branch\n");
    gst_object_unref(sp);
    
    dump_dot(ctx.pipeline, uid);
    ctx.rec_attached = TRUE;
}

static gboolean stop_rec_teardown(gpointer d) {
    (void)d;
    for (int i=0; i < rec_n_elems; i++) {
        gst_element_set_state(rec_elems[i], GST_STATE_NULL);
        gst_bin_remove(GST_BIN(ctx.pipeline), rec_elems[i]);
    }
    rec_n_elems = 0;
    ctx.rec_attached = FALSE;
    g_print("REC TEARDOWN COMPLETE.\n");
    return G_SOURCE_REMOVE;
}

static GstPadProbeReturn rec_unlink_probe(GstPad *p, GstPadProbeInfo *i, gpointer d) {
    (void)i; (void)d;
    if (rec_n_elems == 0) return GST_PAD_PROBE_REMOVE;
    GstPad *sp = gst_element_get_static_pad(rec_elems[0], "sink");
    if(!sp) return GST_PAD_PROBE_REMOVE;
    gst_pad_unlink(p, sp); 
    gst_element_release_request_pad(ctx.tee, p);
    gst_pad_send_event(sp, gst_event_new_eos()); 
    gst_object_unref(sp);
    g_timeout_add(3000, stop_rec_teardown, NULL);
    return GST_PAD_PROBE_REMOVE;
}

static void stop_rec_branch(void) {
    if(!ctx.rec_attached) return;
    g_print("Stopping recording...\n");
    gst_pad_add_probe(ctx.rec_teepad, GST_PAD_PROBE_TYPE_IDLE, rec_unlink_probe, NULL, NULL);
}

static gboolean monitor_cb(gpointer d) {
    (void)d;
    g_print("[MONITOR] RTSP-frames:%d REC-frames:%d\n", ctx.rtsp_frame_count, ctx.rec_frame_count);
    return G_SOURCE_CONTINUE;
}

static void *input_thread(void *d) {
    (void)d;
    char l[512];
    g_print("\n=== Interactive Commands ===\n");
    g_print("  r           - Add RTSP branch\n");
    g_print("  d [opts]    - Add mapping branch (e.g. d fps=20 bitrate=2000000 codec=h265)\n");
    g_print("  s           - Stop branch\n");
    g_print("  q           - Quit\n");
    while (fgets(l, sizeof(l), stdin)) {
        if (l[0] == 'q') { g_main_loop_quit(ctx.loop); break; }
        else if (l[0] == 'r') add_rtsp_branch();
        else if (l[0] == 'd') add_rec_branch(strlen(l) > 2 ? l+2 : "");
        else if (l[0] == 's') stop_rec_branch();
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv); g_setenv("GST_DEBUG_DUMP_DOT_DIR", DOT_DIR, TRUE);
    build_capture_pipeline(argc > 1 ? argv[1] : DEFAULT_DEVICE);
    start_rtsp_server();
    gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING);
    g_timeout_add(3000, monitor_cb, NULL);
    g_timeout_add_seconds(10, dot_dump_cb, NULL);
    pthread_t t; pthread_create(&t, NULL, input_thread, NULL);
    ctx.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(ctx.loop);
    return 0;
}
