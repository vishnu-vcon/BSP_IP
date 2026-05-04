#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include "uvc_sei_injection.h"

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data) {
    (void)factory; (void)user_data;
    GstElement *element = gst_rtsp_media_get_element(media);
    
    GstElement *cam = gst_bin_get_by_name(GST_BIN(element), "cam_src");
    if (cam) {
        GstPad *cam_src_pad = gst_element_get_static_pad(cam, "src");
        if (cam_src_pad) {
            gst_pad_add_probe(cam_src_pad, GST_PAD_PROBE_TYPE_BUFFER, camera_src_probe, NULL, NULL);
            gst_object_unref(cam_src_pad);
            g_print("System Link: Camera Source probe attached for zero-latency IMU sampling.\n");
        }
        gst_object_unref(cam);
    }

    GstElement *encoder = gst_bin_get_by_name(GST_BIN(element), "hw_enc");
    if (encoder) {
        GstPad *src_pad = gst_element_get_static_pad(encoder, "src");
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, sei_injection_probe, GINT_TO_POINTER(0), NULL);
        gst_object_unref(src_pad);
        gst_object_unref(encoder);
        g_print("System Link: SEI Injection probe attached to encoder src pad.\n");
    }
    gst_object_unref(element);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    imu_open();

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstRTSPServer *server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server, "0.0.0.0"); 
    gst_rtsp_server_set_service(server, "8554");

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    gst_rtsp_media_factory_set_launch(factory,
        "( v4l2src name=cam_src device=/dev/video3 do-timestamp=true ! "
        "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "videoconvert ! video/x-raw,width=1920,height=1080,framerate=30/1,colorimetry=bt709,format=I420 ! "
        "v4l2h264enc name=hw_enc extra-controls=\"controls,video_bitrate=10000000,video_gop_size=30,video_b_frames=0\" ! "
        "video/x-h264,profile=high,level=(string)4.2 ! "
        "h264parse ! rtph264pay name=pay0 pt=96 )");
    
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(media_configure_cb), NULL);
    gst_rtsp_mount_points_add_factory(mounts, "/stream", factory);
    gst_object_unref(mounts);

    gst_rtsp_server_attach(server, NULL);
    g_print("RTSP ONLY Server active.\n");
    g_main_loop_run(loop);
    return 0;
}
