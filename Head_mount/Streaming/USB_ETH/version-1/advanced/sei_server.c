#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

/* Use the exact same injection logic as UVC */
#include "uvc_sei_injection.h"

/* Link the probe when the RTSP server spins up the pipeline */
static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data) {
    (void)factory; (void)user_data;

    GstElement *element = gst_rtsp_media_get_element(media);
    
    /* Attach camera source probe for zero-latency IMU sampling.
     * This probe MUST fire BEFORE the encoder's src probe for the same frame.
     * Ordering is guaranteed because:
     *   - Without queue: single streaming thread, chain-function order
     *   - With queue: buffer passes through queue mutex before reaching encoder
     */
    GstElement *cam = gst_bin_get_by_name(GST_BIN(element), "cam_src");
    if (cam) {
        GstPad *cam_src_pad = gst_element_get_static_pad(cam, "src");
        if (cam_src_pad) {
            gst_pad_add_probe(cam_src_pad, GST_PAD_PROBE_TYPE_BUFFER, camera_src_probe, NULL, NULL);
            gst_object_unref(cam_src_pad);
            g_print("System Link: Camera Source probe attached for zero-latency IMU sampling.\n");
        }
        gst_object_unref(cam);
    } else {
        g_printerr("System Link: FATAL — failed to find cam_src element\n");
    }

    /* Attach SEI injection probe on encoder output */
    GstElement *encoder = gst_bin_get_by_name(GST_BIN(element), "hw_enc");
    if (encoder) {
        GstPad *src_pad = gst_element_get_static_pad(encoder, "src");
        /* Pass (gpointer)0 to indicate H.264 format to the injection probe */
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, sei_injection_probe, GINT_TO_POINTER(0), NULL);
        gst_object_unref(src_pad);
        gst_object_unref(encoder);
        g_print("System Link: SEI Injection probe attached to encoder src pad.\n");
    }

    gst_object_unref(element);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    
    const char *ip = "0.0.0.0";
    const char *port = "8554";
    const char *mount = "/stream";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            mount = argv[++i];
        }
    }

    /* Initialize IMU */
    imu_open();

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstRTSPServer *server = gst_rtsp_server_new();
    
    /* Bind to requested interface and port */
    gst_rtsp_server_set_address(server, ip); 
    gst_rtsp_server_set_service(server, port);

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    /* Pipeline with queue for decoupled capture/encode:
     *
     * v4l2src (cam_src) → queue → videoconvert → v4l2h264enc (hw_enc) → h264parse → rtph264pay
     *        ↑                                              ↑
     *  camera_src_probe                              sei_injection_probe
     *  (writes IMU to                                (reads IMU from
     *   circular buffer)                              circular buffer,
     *                                                 builds SEI NAL)
     *
     * The queue prevents v4l2src from stalling if the encoder is slow,
     * while maintaining probe ordering (buffer traverses queue before encoder).
     */
    /* Pipeline: v4l2src → queue → videoconvert → v4l2h264enc (10 Mbps) → h264parse → RTP
     *
     * Bitrate: 10 Mbps (10,000,000 bps) via V4L2 extra-controls.
     * The i.MX8MP VPU encoder uses V4L2 CID 'video_bitrate' (in bps).
     * At 1080p30, 10 Mbps yields high quality H.264 High profile output.
     * Minimum requirement: >= 9 Mbps per spec.
     */
    gst_rtsp_media_factory_set_launch(factory,
        "( v4l2src name=cam_src device=/dev/video3 do-timestamp=true ! "
        "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "videoconvert ! video/x-raw,width=1920,height=1080,framerate=30/1 ! "
        "v4l2h264enc name=hw_enc extra-controls=\"controls,video_bitrate=10000000\" ! "
        "video/x-h264,profile=high ! "
        "h264parse ! rtph264pay name=pay0 pt=96 )");
    
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(media_configure_cb), NULL);
    gst_rtsp_mount_points_add_factory(mounts, mount, factory);
    gst_object_unref(mounts);

    gst_rtsp_server_attach(server, NULL);
    g_print("Advanced RTSP Server active at rtsp://%s:%s%s\n", ip, port, mount);
    g_print("  Resolution : 1920x1080 @ 30fps\n");
    g_print("  Bitrate    : 10 Mbps (CBR via V4L2 extra-controls)\n");
    g_print("  IMU sync   : < 1ms (camera-probe inline IOCTL)\n");
    g_print("  SEI payload: %zu bytes (UUID+SeiMetadata)\n", 16 + sizeof(SeiMetadata));
    g_main_loop_run(loop);

    return 0;
}
