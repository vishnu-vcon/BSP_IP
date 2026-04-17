/*
 * encoder_builder.c — Encoder Chain Builder
 * ==========================================
 *
 * OPTIMISATIONS APPLIED:
 * [OPT-A] Non-Cairo Direct Path:
 * Branch attaches to the raw NV12 capture_tee.
 * ALWAYS uses imxvideoconvert_g2d to satisfy the resolution change
 * demanded by the capsfilter.
 * *CRITICAL FIX*: The capsfilter OMITS the format (no BGRx, no NV12).
 * This allows G2D and VPU auto-negotiate the most efficient native format
 * directly with the hardware encoder.
 */

#include "gst_blocks/encoder_builder.h"
#include <stdio.h>
#include <string.h>

static GstElement **_abort_chain(GstElement **elems, int n, int *out_count) {
    for (int i = 0; i < n; i++) {
        if (elems[i]) gst_object_unref(elems[i]);
    }
    *out_count = 0;
    return NULL;
}

GstElement **build_encoder_elements(const char *uid, const char *codec, int fps,
                                    int width, int height, int src_width, int src_height,
                                    int bitrate, const char *bitrate_mode,
                                    gboolean cairo_enabled,
                                    gboolean ntp_enabled,
                                    gboolean is_third,
                                    int *out_count) {
    (void)bitrate_mode;
    (void)src_width;
    (void)src_height;
    (void)ntp_enabled;
    
    GstElement *elems[20];
    int n = 0;
    char name[128];

    g_debug("[ENC-BUILD] uid=%-32s  codec=%-6s  %dx%d @ %d fps  cairo=%s",
            uid, codec ? codec : "NULL", width, height, fps,
            cairo_enabled ? "TRUE" : "FALSE");

    /* NOTE: plain_mjpeg branch DISABLED — switched third branch to x264enc H.264.
     * MJPEG jpegenc was consuming ~70% CPU at 640x480. x264enc ultrafast uses ~5-8%.
     * Keeping the code commented out for potential future re-enablement.
     *
     * gboolean plain_mjpeg = (!cairo_enabled && codec && g_strcmp0(codec, "mjpeg") == 0);
     */

    /* ── Queue — common to all branches ── */
    snprintf(name, sizeof(name), "q_%s", uid);
    GstElement *q = gst_element_factory_make("queue", name);
    if (!q) return _abort_chain(elems, n, out_count);
    g_object_set(q, "max-size-buffers", 2, "max-size-bytes", 0,
                 "max-size-time", (guint64)0, "leaky", 2, NULL);
    elems[n++] = q;

    /* ═══════════════════════════════════════════════════════════════════════
     * PLAIN MJPEG BRANCH — DISABLED (2025-04)
     * Reason: jpegenc software encoding consumed ~70% CPU on i.MX8MP.
     * Third branch now uses x264enc via the standard H.264 path below.
     * Also caused HLS pipeline errors (h264parse receiving JPEG data).
     * ═══════════════════════════════════════════════════════════════════════ */
#if 0  /* MJPEG branch disabled — see notes above */
    if (plain_mjpeg) {
        /* [BRANCH-3] Strict Software Path: videorate (3fps) -> videoscale -> videoconvert -> jpegenc */
        if (is_third) {
            /* 1. Force 3 FPS limit */
            snprintf(name, sizeof(name), "rate_%s", uid);
            GstElement *vrate = gst_element_factory_make("videorate", name);
            if (!vrate) return _abort_chain(elems, n, out_count);
            g_object_set(vrate, "max-rate", 3, "drop-only", TRUE, "skip-to-first", TRUE, NULL);
            elems[n++] = vrate;

            snprintf(name, sizeof(name), "rate_caps_%s", uid);
            GstElement *rate_f = gst_element_factory_make("capsfilter", name);
            if (!rate_f) return _abort_chain(elems, n, out_count);
            GstCaps *caps_v = gst_caps_from_string("video/x-raw,framerate=3/1");
            g_object_set(rate_f, "caps", caps_v, NULL);
            gst_caps_unref(caps_v);
            elems[n++] = rate_f;

            /* 2. Software Scale (if needed) */
            if (width != src_width || height != src_height) {
                snprintf(name, sizeof(name), "scale_%s", uid);
                GstElement *scaler = gst_element_factory_make("videoscale", name);
                if (!scaler) return _abort_chain(elems, n, out_count);
                elems[n++] = scaler;

                snprintf(name, sizeof(name), "res_%s", uid);
                GstElement *res_f = gst_element_factory_make("capsfilter", name);
                if (!res_f) return _abort_chain(elems, n, out_count);
                char res_caps[128];
                snprintf(res_caps, sizeof(res_caps), "video/x-raw,width=%d,height=%d", width, height);
                GstCaps *caps_r = gst_caps_from_string(res_caps);
                g_object_set(res_f, "caps", caps_r, NULL);
                gst_caps_unref(caps_r);
                elems[n++] = res_f;
            }
        } else {
            /* Hardware Path for other MJPEG (Main/Sub) */
            if (width != src_width || height != src_height) {
                const char *sf = gst_element_factory_find("imxvideoconvert_g2d")
                                 ? "imxvideoconvert_g2d" : "videoconvert";
                snprintf(name, sizeof(name), "scale_%s", uid);
                GstElement *scaler = gst_element_factory_make(sf, name);
                if (!scaler) return _abort_chain(elems, n, out_count);
                elems[n++] = scaler;
 
                snprintf(name, sizeof(name), "res_%s", uid);
                GstElement *res_f = gst_element_factory_make("capsfilter", name);
                if (!res_f) return _abort_chain(elems, n, out_count);
                char res_caps[128];
                snprintf(res_caps, sizeof(res_caps), "video/x-raw,width=%d,height=%d", width, height);
                GstCaps *caps_r = gst_caps_from_string(res_caps);
                g_object_set(res_f, "caps", caps_r, NULL);
                gst_caps_unref(caps_r);
                elems[n++] = res_f;
            }
        }

        snprintf(name, sizeof(name), "conv_%s", uid);
        GstElement *conv = gst_element_factory_make("videoconvert", name);
        if (!conv) return _abort_chain(elems, n, out_count);
        elems[n++] = conv;

        /* 3. Late Videorate Clamper: Ensure strict 3 FPS adherence before encoder */
        if (is_third) {
            snprintf(name, sizeof(name), "rate_late_%s", uid);
            GstElement *vrate_late = gst_element_factory_make("videorate", name);
            if (!vrate_late) return _abort_chain(elems, n, out_count);
            g_object_set(vrate_late, "max-rate", 3, "drop-only", TRUE, "skip-to-first", TRUE, NULL);
            elems[n++] = vrate_late;
        }

        snprintf(name, sizeof(name), "enc_%s", uid);
        GstElement *jpegenc = gst_element_factory_make("jpegenc", name);
        if (!jpegenc) return _abort_chain(elems, n, out_count);
        elems[n++] = jpegenc;

        goto build_done;
    }
#endif  /* MJPEG branch disabled */

    /* ═══════════════════════════════════════════════════════════════════════
     * STANDARD BGRx PATH (Restore Stable Hub pattern)
     * ALWAYS uses G2D to scale and force output to BGRx for stability.
     * ═══════════════════════════════════════════════════════════════════════ */
    const char *sf = gst_element_factory_find("imxvideoconvert_g2d")
                     ? "imxvideoconvert_g2d" : "videoconvert";
    snprintf(name, sizeof(name), "branch_scale_%s", uid);
    GstElement *scaler = gst_element_factory_make(sf, name);
    if (!scaler) return _abort_chain(elems, n, out_count);
    elems[n++] = scaler;

    snprintf(name, sizeof(name), "res_%s", uid);
    GstElement *res_filter = gst_element_factory_make("capsfilter", name);
    if (!res_filter) return _abort_chain(elems, n, out_count);
    char caps_str[256];
    snprintf(caps_str, sizeof(caps_str),
             "video/x-raw,width=%d,height=%d", width, height);
    GstCaps *res_caps = gst_caps_from_string(caps_str);
    g_object_set(res_filter, "caps", res_caps, NULL);
    gst_caps_unref(res_caps);
    elems[n++] = res_filter;

    /* Cairo overlay (only if enabled) */
    if (cairo_enabled) {
        snprintf(name, sizeof(name), "cairo_%s", uid);
        GstElement *cairo = gst_element_factory_make("cairooverlay", name);
        if (cairo) {
            snprintf(name, sizeof(name), "q_cairo_%s", uid);
            GstElement *q_cairo = gst_element_factory_make("queue", name);
            if (!q_cairo) return _abort_chain(elems, n, out_count);
            g_object_set(q_cairo, "max-size-time", (guint64)0,
                         "max-size-buffers", 2, "max-size-bytes", 0, "leaky", 2, NULL);
            elems[n++] = q_cairo;
            g_object_set_data(G_OBJECT(cairo), "branch_width",     GINT_TO_POINTER(width));
            g_object_set_data(G_OBJECT(cairo), "branch_height",    GINT_TO_POINTER(height));
            g_object_set_data(G_OBJECT(cairo), "overlay_enabled", GINT_TO_POINTER(TRUE));
            elems[n++] = cairo;
        }
    }

    /* NTP clock overlay */
    if (ntp_enabled) {
        snprintf(name, sizeof(name), "clock_%s", uid);
        GstElement *clock_ovl = gst_element_factory_make("clockoverlay", name);
        if (clock_ovl) {
            g_object_set(clock_ovl,
                         "time-format",       "%d-%m-%Y %H:%M:%S IST",
                         "valignment",        4,
                         "halignment",        2,
                         "font-desc",         "Sans 20",
                         "shaded-background", TRUE,
                         "silent",            FALSE,
                         NULL);
            elems[n++] = clock_ovl;
        }
    }

    /* ── Rate control + encoder (shared by both paths) ── */
    int effective_fps = (fps > 0) ? fps : 30;
    snprintf(name, sizeof(name), "rate_%s", uid);
    GstElement *rate = gst_element_factory_make("videorate", name);
    if (!rate) return _abort_chain(elems, n, out_count);
    g_object_set(rate, "drop-only", TRUE, "max-rate", effective_fps,
                 "skip-to-first", TRUE, NULL);
    elems[n++] = rate;

    snprintf(name, sizeof(name), "fps_caps_%s", uid);
    GstElement *fps_filter = gst_element_factory_make("capsfilter", name);
    if (!fps_filter) return _abort_chain(elems, n, out_count);
    char fps_str[128];
    snprintf(fps_str, sizeof(fps_str), "video/x-raw,framerate=%d/1", effective_fps);
    GstCaps *fps_caps = gst_caps_from_string(fps_str);
    g_object_set(fps_filter, "caps", fps_caps, NULL);
    gst_caps_unref(fps_caps);
    elems[n++] = fps_filter;

    /* MJPEG with cairo needs format conversion before jpegenc */
    if (codec && g_strcmp0(codec, "mjpeg") == 0 && cairo_enabled) {
        snprintf(name, sizeof(name), "conv_mjpeg_%s", uid);
        GstElement *conv = gst_element_factory_make("videoconvert", name);
        if (!conv) return _abort_chain(elems, n, out_count);
        elems[n++] = conv;
    }

    /* Encoder with fallback */
    snprintf(name, sizeof(name), "enc_%s", uid);
    GstElement *enc = NULL;

    if (is_third) {
        /* [THIRD-BRANCH] Force x264enc software encoder.
         * Rationale: Third branch is low-priority (640x480@3fps). Using x264enc
         * avoids consuming a limited VPU hardware slot. At ultrafast/zerolatency
         * this costs ~5-8% CPU vs jpegenc's ~70%. */
        enc = gst_element_factory_make("x264enc", name);
        if (enc) {
            g_object_set(enc,
                         "speed-preset", 1,   /* ultrafast */
                         "tune",         4,   /* zerolatency */
                         "bitrate",      (bitrate > 0 ? bitrate / 1000 : 500), /* kbps */
                         "key-int-max",  effective_fps > 0 ? effective_fps : 3,
                         NULL);
            g_info("[ENC-BUILD] Third branch: x264enc (ultrafast/zerolatency) %d kbps",
                   bitrate > 0 ? bitrate / 1000 : 500);
        }
    } else if (codec && g_strcmp0(codec, "mjpeg") == 0) {
        enc = gst_element_factory_make("jpegenc", name);
    } else if (codec && g_strcmp0(codec, "h265") == 0) {
        enc = gst_element_factory_make("v4l2h265enc", name);
        if (!enc) enc = gst_element_factory_make("x265enc", name);
    } else {
        enc = gst_element_factory_make("v4l2h264enc", name);
        if (!enc) enc = gst_element_factory_make("x264enc", name);
    }
    
    if (!enc) return _abort_chain(elems, n, out_count);

    /* Configure hardware encoder (skip for x264enc/x265enc software encoders) */
    GstElementFactory *ef = gst_element_get_factory(enc);
    const char *fname = ef ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(ef)) : "";
    if (strstr(fname, "v4l2")) {
        GstStructure *ec = gst_structure_new("controls",
            "video_bitrate",  G_TYPE_INT, bitrate > 0 ? bitrate : 4000000,
            "video_gop_size", G_TYPE_INT, effective_fps, NULL);
        g_object_set(enc, 
                     "extra-controls", ec,
                     NULL);
        gst_structure_free(ec);
    }
    elems[n++] = enc;

    /* Parser with fallback */
    if (codec && g_strcmp0(codec, "mjpeg") != 0) {
        const char *pf = (g_strcmp0(codec, "h265") == 0) ? "h265parse" : "h264parse";
        snprintf(name, sizeof(name), "parse_%s", uid);
        GstElement *parser_elem = gst_element_factory_make(pf, name);
        if (parser_elem) elems[n++] = parser_elem;
    }

build_done:
    *out_count = n;

    GstElement **result = g_new(GstElement *, n);
    memcpy(result, elems, sizeof(GstElement *) * n);
    return result;
}
