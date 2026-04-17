/*
 * encoder_builder.h — Encoder Chain Builder
 * ==========================================
 * Port of: unified_engine.py — build_encoder_elements() (lines 417–567)
 *
 * CPU-copy / G2D optimisations (2025-04):
 *
 *  [OPT-A] NV12 direct path (cairo_enabled == FALSE):
 *    When no overlay is needed the branch is attached to the raw NV12 capture
 *    tee.  The chain becomes:
 *      queue → [videoconvert/imxvideoconvert_g2d → capsfilter(BGRx)] → videorate
 *              → capsfilter(fps) → v4l2h264enc → h264parse
 *    For the NV12 path the scaler converts NV12→BGRx only once in the branch,
 *    skipping the global G2D entirely for this branch's pixel data.
 *
 *  [OPT-B] Skip per-branch G2D when output resolution == tee output resolution:
 *    If the requested (width,height) equals the BGRx tee output (src_width,src_height), there is
 *    nothing to scale.  The imxvideoconvert_g2d element is omitted and a plain
 *    capsfilter asserts the format constraint for GStreamer negotiation.
 *
 *  [OPT-C] When a branch DOES need a downscale (e.g. 1080p → 720p) the G2D
 *    element is retained — hardware scaling is still preferable to software.
 */

#ifndef SMARTIP_ENCODER_BUILDER_H
#define SMARTIP_ENCODER_BUILDER_H

#include <gst/gst.h>
#include <glib.h>

/*
 * build_encoder_elements — Build a processing + encoder chain for a dynamic branch.
 *
 * Returns a heap-allocated array of GstElement pointers (length *out_count).
 * Returns NULL on fatal error (caller must not add any elements).
 *
 * @param uid            Unique identifier for element naming
 * @param codec          "h264", "h265", or "mjpeg"
 * @param fps            Target frame rate
 * @param width          Target width
 * @param height         Target height
 * @param src_width      BGRx tee output width  (used for skip-scale decision)
 * @param src_height     BGRx tee output height (used for skip-scale decision)
 * @param bitrate        Target bitrate (bps)
 * @param bitrate_mode   "vbr" or "cbr"
 * @param cairo_enabled  Include cairooverlay + clockoverlay (BGRx path).
 *                       FALSE → attach to NV12 tee, no overlay elements.
 * @param ntp_enabled    Include clockoverlay (only when cairo_enabled == TRUE)
 * @param out_count      Output: number of elements in the returned array
 */
GstElement **build_encoder_elements(const char *uid,
                                    const char *codec,
                                    int fps,
                                    int width, int height,
                                    int src_width, int src_height,
                                    int bitrate,
                                    const char *bitrate_mode,
                                    gboolean cairo_enabled,
                                    gboolean ntp_enabled,
                                    gboolean is_third,
                                    int *out_count);

#endif /* SMARTIP_ENCODER_BUILDER_H */
