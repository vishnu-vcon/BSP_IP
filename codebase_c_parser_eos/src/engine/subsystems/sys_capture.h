/*
 * capture_pipeline.h — Capture Pipeline + Dynamic Branch Management
 * ==================================================================
 * Port of: unified_engine.py — CapturePipeline class (lines 150–312)
 *                              DynamicBranch class   (lines 318–411)
 *                              BranchConfig class    (lines 69–83)
 *
 * Architecture per lens:
 *   v4l2src → capsfilter(NV12) → capture_tee
 *     ├── [permanent] queue → fakesink           ← keeps V4L2 pool flowing
 *     ├── [permanent] queue → imxvideoconvert_g2d → capsfilter(BGRx) → capture_bgrx_tee
 *     │     ├── [permanent] queue → fakesink2
 *     │     ├── [permanent] queue → snap_sink (appsink)
 *     │     └── [on-demand] RTSP/HLS shared encoder branches (BGRx path, cairo capable)
 *     └── [on-demand] Recording / AI / MJPEG-plain branches (NV12 path, no overlay)
 *
 * Branch routing decision (set at add_branch time via name prefix):
 *   NV12 tee  → names containing "rec_", "_third", "ai_shm"
 *   BGRx tee  → all other shared encoder branches
 *
 * CPU-copy elimination strategy (2025-04):
 *   - No per-branch imxvideoconvert_g2d when branch resolution == tee resolution
 *     (tee_w / tee_h stored in CapturePipeline for comparison by encoder_builder)
 *   - NV12-path branches skip the global BGRx G2D entirely (cairo not needed)
 *   - Buffer delivery uses gst_buffer_copy_region (header-only, zero pixel copy)
 *
 * Defaults:
 *   Bitrate: 4Mbps  |  Bitrate Mode: VBR
 */

#define DEFAULT_BRANCH_BITRATE      4000000
#define DEFAULT_BRANCH_BITRATE_MODE "vbr"
#define DEFAULT_BRANCH_FPS          25
#define DEFAULT_BRANCH_RES          "1920x1080"

#ifndef SMARTIP_CAPTURE_PIPELINE_H
#define SMARTIP_CAPTURE_PIPELINE_H

#include <gst/gst.h>
#include <glib.h>

/* ── Branch Config (port of BranchConfig class) ── */
typedef struct {
    char     resolution[16]; /* e.g. "1920x1080" */
    int      fps;
    char     codec[16];      /* "h264", "h265", "mjpeg" */
    int      width;
    int      height;
    int      bitrate;
    char     bitrate_mode[16];
    gboolean ntp_overlay;      /* STRICT CONFIGURATION: Enable NTP Clock Overlay */
    gboolean overlay_enabled;  /* DYNAMIC TOGGLE: Enable/Disable Cairo Overlay (if present) */
} BranchConfig;

BranchConfig *branch_config_new(const char *resolution, int fps, const char *codec);
void          branch_config_free(BranchConfig *branch_cfg);

/* ── Dynamic Branch (port of DynamicBranch class) ── */
typedef struct _CapturePipeline CapturePipeline;

typedef struct {
    char             name[64];
    CapturePipeline *capture;
    GstElement     **elements;
    int              num_elements;
    GstPad          *teepad;
    gboolean         removing;
    gboolean         use_nv12_tee; /* TRUE → attach to NV12 capture_tee; FALSE → BGRx capture_bgrx_tee */
} DynamicBranch;

/* ── AI Coordinates State ── */
typedef struct {
    float left;
    float top;
    float right;
    float bottom;
    float conf;
    char  label[32];
} AICoord;

typedef struct {
    GMutex   mutex;
    gboolean cairo_capable;
    gboolean overlay_enabled;
    int      ml_w;
    int      ml_h;
    AICoord *coords;
    int      num_coords;
    gint64   coords_update_time;  /* g_get_monotonic_time() when coords last changed */
    /* SHM Persistence */
    gboolean shm_requested;
    char     shm_socket[256];
} AILensState;

/* ── Capture Pipeline (port of CapturePipeline class) ── */
struct _CapturePipeline {
    char          device[64];
    char          caps_str[256];

    /* Resolution of the BGRx tee output — used by encoder_builder to decide
     * whether a per-branch imxvideoconvert_g2d is needed (skip when
     * branch_width == tee_width && branch_height == tee_height). */
    int           tee_width;
    int           tee_height;

    GstElement   *pipeline;
    GstElement   *capture_tee;
    GstElement   *capture_bgrx_tee; /* BGRx tee fed by the global G2D */
    GstElement   *snap_sink;
    GHashTable   *dynamic_branches; /* name → DynamicBranch* */
    GMutex        branch_mutex;     /* Protects dynamic_branches */
    AILensState   ai;
};

/* ── CapturePipeline API ── */
CapturePipeline *capture_pipeline_new  (const char *device, const char *caps, gboolean cairo_capable);
void             capture_pipeline_start(CapturePipeline *capture_pipe);
void             capture_pipeline_stop (CapturePipeline *capture_pipe);
void             capture_pipeline_free (CapturePipeline *capture_pipe);
void             capture_pipeline_dump_dot(CapturePipeline *capture_pipe, const char *name);

GstSample       *capture_pipeline_grab_snapshot(CapturePipeline *capture_pipe);

/* ── Dynamic Branch API ── */
DynamicBranch   *capture_pipeline_add_branch   (CapturePipeline *capture_pipe, const char *name,
                                                 GstElement **elements, int num_elements,
                                                 gboolean use_nv12_tee);
void             capture_pipeline_remove_branch(CapturePipeline *capture_pipe, const char *name);
void             capture_pipeline_attach_ai_shm(CapturePipeline *capture_pipe, const char *socket_path);
void             capture_pipeline_detach_ai_shm(CapturePipeline *capture_pipe);

#endif /* SMARTIP_CAPTURE_PIPELINE_H */
