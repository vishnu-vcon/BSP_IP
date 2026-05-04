#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

typedef struct { float x, y, width, height; } AIData;
typedef struct {
    AIData current_data;
    pthread_mutex_t mutex;
} AppContext;

AppContext app_ctx = { .mutex = PTHREAD_MUTEX_INITIALIZER };

// Simulate AI Data Generation (Control Plane)
void* ai_thread_func(void* arg) {
    float c = 0.0;
    while (1) {
        pthread_mutex_lock(&app_ctx.mutex);
        app_ctx.current_data.x = c;
        pthread_mutex_unlock(&app_ctx.mutex);
        c += 1.0;
        usleep(33000); 
    }
    return NULL;
}

// Emulation Prevention to avoid false start codes
int apply_emulation_prevention(const uint8_t *src, int src_len, uint8_t *dst) {
    int dst_len = 0, zero_count = 0;
    for (int i = 0; i < src_len; i++) {
        if (zero_count == 2 && src[i] <= 0x03) { dst[dst_len++] = 0x03; zero_count = 0; }
        if (src[i] == 0x00) zero_count++; else zero_count = 0;
        dst[dst_len++] = src[i];
    }
    return dst_len;
}

// Data Plane: Injecting SEI right before h264parse
static GstPadProbeReturn sei_injection_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    pthread_mutex_lock(&app_ctx.mutex);
    AIData local_ai = app_ctx.current_data;
    pthread_mutex_unlock(&app_ctx.mutex);

    uint8_t raw_payload[sizeof(AIData)];
    memcpy(raw_payload, &local_ai, sizeof(AIData));

    uint8_t epb_payload[sizeof(AIData) * 2]; 
    int epb_len = apply_emulation_prevention(raw_payload, sizeof(AIData), epb_payload);

    uint8_t sei_header[] = { 0x00, 0x00, 0x00, 0x01, 0x06, 0x05 };
    int total_size = sizeof(sei_header) + epb_len + 1;
    uint8_t *sei_packet = g_malloc(total_size + 1);
    
    int offset = 0;
    memcpy(sei_packet + offset, sei_header, sizeof(sei_header)); offset += sizeof(sei_header);
    sei_packet[offset++] = epb_len;
    memcpy(sei_packet + offset, epb_payload, epb_len); offset += epb_len;
    sei_packet[offset] = 0x80; 

    // Zero-copy prepend
    GstBuffer *sei_buf = gst_buffer_new_wrapped(sei_packet, total_size + 1);
    GstBuffer *new_buffer = gst_buffer_append(sei_buf, gst_buffer_ref(buffer));
    
    GST_PAD_PROBE_INFO_DATA(info) = new_buffer;
    gst_buffer_unref(buffer);

    return GST_PAD_PROBE_OK;
}

// Link the probe when the RTSP server spins up the pipeline
static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data) {
    GstElement *element = gst_rtsp_media_get_element(media);
    
    // Find the encoder and attach to its src pad (which feeds directly into h264parse sink)
    GstElement *encoder = gst_bin_get_by_name(GST_BIN(element), "hw_enc");
    if (encoder) {
        GstPad *src_pad = gst_element_get_static_pad(encoder, "src");
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, sei_injection_probe, NULL, NULL);
        gst_object_unref(src_pad);
        gst_object_unref(encoder);
        g_print("System Link: SEI Injection probe attached to encoder src pad.\n");
    }
    gst_object_unref(element);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    
    pthread_t thread;
    pthread_create(&thread, NULL, ai_thread_func, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstRTSPServer *server = gst_rtsp_server_new();
    // Bind to the CDC-NCM interface we setup previously
    gst_rtsp_server_set_address(server, "10.0.0.1"); 
    gst_rtsp_server_set_service(server, "8554");

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    // The whole system flow: Camera -> HW Encoder -> h264parse -> RTP Payloader
    gst_rtsp_media_factory_set_launch(factory,
        "( v4l2src device=/dev/video4 ! videoconvert ! video/x-raw,width=1280,height=720 ! "
        "v4l2h264enc name=hw_enc ! h264parse ! rtph264pay name=pay0 pt=96 )");
    
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(media_configure_cb), NULL);
    gst_rtsp_mount_points_add_factory(mounts, "/stream", factory);
    gst_object_unref(mounts);

    gst_rtsp_server_attach(server, NULL);
    g_print("RTSP Server active at rtsp://10.0.0.1:8554/stream\n");
    g_main_loop_run(loop);

    return 0;
}
