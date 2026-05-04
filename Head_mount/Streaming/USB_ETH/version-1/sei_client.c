#include <gst/gst.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

typedef struct { float x, y, width, height; } AIData;

int remove_emulation_prevention(const uint8_t *src, int src_len, uint8_t *dst) {
    int dst_len = 0, zero_count = 0;
    for (int i = 0; i < src_len; i++) {
        if (zero_count == 2 && src[i] == 0x03) { zero_count = 0; continue; }
        if (src[i] == 0x00) zero_count++; else zero_count = 0;
        dst[dst_len++] = src[i];
    }
    return dst_len;
}

// Data Plane: Extracting SEI at h264parse sink
static GstPadProbeReturn sei_extraction_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        uint8_t *data = map.data;
        gsize size = map.size;

        for (gsize i = 0; i < size - 5; i++) {
            if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && 
                data[i+3] == 0x01 && data[i+4] == 0x06 && data[i+5] == 0x05) { 
                
                int payload_offset = i + 6;
                uint8_t epb_len = data[payload_offset];
                payload_offset++; 

                uint8_t epb_payload[256]; 
                memcpy(epb_payload, &data[payload_offset], epb_len);

                uint8_t raw_payload[sizeof(AIData)];
                if (remove_emulation_prevention(epb_payload, epb_len, raw_payload) == sizeof(AIData)) {
                    AIData received_ai;
                    memcpy(&received_ai, raw_payload, sizeof(AIData));
g_print("Extracted SEI Box - X: %.2f, Y: %.2f, W: %.2f, H: %.2f\n", 
        received_ai.x, received_ai.y, received_ai.width, received_ai.height);
                }
                break; 
            }
        }
        gst_buffer_unmap(buffer, &map);
    }
    return GST_PAD_PROBE_OK;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    GError *error = NULL;
    // The RTSP network flow into the local parsing and decoding pipeline
    GstElement *pipeline = gst_parse_launch(
        "rtspsrc location=rtsp://10.0.0.1:8554/stream ! rtph264depay ! "
        "h264parse name=client_parser ! video/x-h264,stream-format=byte-stream ! "
        "avdec_h264 ! videoconvert ! autovideosink sync=false", 
        &error);

    if (error) { g_printerr("Pipeline error: %s\n", error->message); return -1; }

    // Link the extraction probe to the parser's sink pad
    GstElement *parser = gst_bin_get_by_name(GST_BIN(pipeline), "client_parser");
    if (parser) {
        GstPad *sink_pad = gst_element_get_static_pad(parser, "sink");
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, sei_extraction_probe, NULL, NULL);
        gst_object_unref(sink_pad);
        gst_object_unref(parser);
        g_print("System Link: Extraction probe attached to h264parse sink pad.\n");
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 0;
}
