#include <gst/gst.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "sei_metadata.h"

static int g_verbose = 0;

/* Expected UUID — must match the server's UUID */
static const uint8_t EXPECTED_UUID[16] = {
    0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7,
    0x96, 0x2c, 0xd3, 0x9c, 0x24, 0x5a, 0xc8, 0x1a
};

/* Reverse Emulation Prevention (EPB removal) */
static int remove_emulation_prevention(const uint8_t *src, int src_len, uint8_t *dst) {
    int dst_len = 0, zero_count = 0;
    for (int i = 0; i < src_len; i++) {
        if (zero_count == 2 && src[i] == 0x03) {
            zero_count = 0;
            continue;
        }
        if (src[i] == 0x00) zero_count++;
        else                zero_count = 0;
        dst[dst_len++] = src[i];
    }
    return dst_len;
}

/* GStreamer probe to extract SEI NAL unit */
static GstPadProbeReturn sei_extraction_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)pad; (void)user_data;

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        uint8_t *data = map.data;
        gsize size = map.size;

        for (gsize i = 0; i < size - 6; i++) {
            int is_h264_sei = (data[i] == 0x00 && data[i+1] == 0x00 &&
                               data[i+2] == 0x00 && data[i+3] == 0x01 &&
                               data[i+4] == 0x06 && data[i+5] == 0x05);

            int is_hevc_sei = (i < size - 7) &&
                              (data[i] == 0x00 && data[i+1] == 0x00 &&
                               data[i+2] == 0x00 && data[i+3] == 0x01 &&
                               data[i+4] == 0x4E && data[i+5] == 0x01 &&
                               data[i+6] == 0x05);

            if (is_h264_sei || is_hevc_sei) {
                int payload_offset = is_hevc_sei ? (i + 7) : (i + 6);

                /* Decode multi-byte payload size (RBSP size per H.264 spec) */
                int payload_size = 0;
                while (payload_offset < (int)size && data[payload_offset] == 0xFF) {
                    payload_size += 255;
                    payload_offset++;
                }
                if (payload_offset < (int)size) {
                    payload_size += data[payload_offset];
                    payload_offset++;
                }

                if (payload_offset + payload_size > (int)size)
                    break;

                /* The bitstream from payload_offset contains EBSP (with EPB bytes).
                 * We need to scan forward to find all EBSP bytes for this payload.
                 * Since payloadSize is the RBSP size, the EBSP may be slightly larger
                 * due to inserted 0x03 bytes. We scan up to the next start code or
                 * end of buffer to capture the full EBSP region. */
                int ebsp_end = payload_offset;
                int rbsp_decoded = 0;
                int zero_count = 0;

                /* Walk the EBSP, counting how many RBSP bytes we've decoded */
                while (ebsp_end < (int)size && rbsp_decoded < payload_size) {
                    uint8_t byte = data[ebsp_end];
                    if (zero_count == 2 && byte == 0x03) {
                        /* This is an emulation prevention byte — skip it in RBSP count */
                        ebsp_end++;
                        zero_count = 0;
                        continue;
                    }
                    if (byte == 0x00) zero_count++;
                    else              zero_count = 0;
                    rbsp_decoded++;
                    ebsp_end++;
                }

                int ebsp_len = ebsp_end - payload_offset;

                /* Remove EPB to recover raw RBSP = UUID(16) + SeiMetadata */
                uint8_t rbsp[16 + sizeof(SeiMetadata) + 16]; /* generous buffer */
                int decoded_len = remove_emulation_prevention(
                    &data[payload_offset], ebsp_len, rbsp);

                /* Validate: need at least UUID(16) + SeiMetadata */
                if (decoded_len < 16 + (int)sizeof(SeiMetadata))
                    break;

                /* Verify UUID matches our expected UUID */
                if (memcmp(rbsp, EXPECTED_UUID, 16) != 0)
                    break;

                /* Parse SeiMetadata from RBSP bytes AFTER the 16-byte UUID */
                SeiMetadata meta;
                memcpy(&meta, rbsp + 16, sizeof(SeiMetadata));

                /* Validate magic */
                if (meta.magic != SEI_METADATA_MAGIC)
                    break;

                if (g_verbose) {
                    g_print("\n── SEI Frame #%u ────────────────────────────\n", meta.frame_id);
                    g_print("  PTS       : %lu ns (camera)\n", (unsigned long)meta.frame_pts_ns);
                    g_print("  IMU time  : %lu ns (monotonic)\n", (unsigned long)meta.imu_timestamp_ns);
                    g_print("  Delta     : %ld ns\n",
                            (long)(meta.imu_timestamp_ns - meta.frame_pts_ns));
                    g_print("  UTC       : %lu us\n", (unsigned long)meta.utc_timestamp_us);
                    g_print("  Accel     : X:%d mg  Y:%d mg  Z:%d mg\n",
                            meta.accel_x_mg, meta.accel_y_mg, meta.accel_z_mg);
                    g_print("  Gyro      : X:%d mdps  Y:%d mdps  Z:%d mdps\n",
                            meta.gyro_x_mdps, meta.gyro_y_mdps, meta.gyro_z_mdps);
                    g_print("  Mag       : X:%d uT  Y:%d uT  Z:%d uT\n",
                            meta.mag_x_ut, meta.mag_y_ut, meta.mag_z_ut);
                    g_print("  Device    : %.16s | Env:%d | %.4f°N, %.4f°E\n",
                            meta.device_serial, meta.environment_type,
                            meta.latitude_e7 / 1e7, meta.longitude_e7 / 1e7);
                    g_print("  Session   : %.16s\n", meta.session_id);
                    g_print("─────────────────────────────────────────────\n");
                } else {
                    g_print("\rSEI #%u | A(%d,%d,%d)mg G(%d,%d,%d)mdps M(%d,%d,%d)uT | D:%ldns   ",
                            meta.frame_id,
                            meta.accel_x_mg, meta.accel_y_mg, meta.accel_z_mg,
                            meta.gyro_x_mdps, meta.gyro_y_mdps, meta.gyro_z_mdps,
                            meta.mag_x_ut, meta.mag_y_ut, meta.mag_z_ut,
                            (long)(meta.imu_timestamp_ns - meta.frame_pts_ns));
                    fflush(stdout);
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

    const char *ip = "10.0.0.1";
    const char *port = "8554";
    const char *mount = "/stream";

    /* Parse flags */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            mount = argv[++i];
        }
    }

    char rtsp_url[256];
    snprintf(rtsp_url, sizeof(rtsp_url), "rtsp://%s:%s%s", ip, port, mount);

    g_print("CDC-NCM SEI Metadata Extractor (v%d)\n", SEI_METADATA_VERSION);
    g_print("  URL    : %s\n", rtsp_url);
    g_print("  Verbose: %s\n", g_verbose ? "yes" : "no (use -v for detail)");
    g_print("  Payload: UUID(16) + %zu bytes (SeiMetadata) = %zu bytes RBSP\n\n",
            sizeof(SeiMetadata), 16 + sizeof(SeiMetadata));

    char pipeline_str[512];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "rtspsrc location=%s ! rtph264depay ! "
        "h264parse name=client_parser ! video/x-h264,stream-format=byte-stream ! "
        "avdec_h264 ! videoconvert ! autovideosink sync=false", 
        rtsp_url);

    GError *error = NULL;
    /* The RTSP network flow into the local parsing and decoding pipeline */
    GstElement *pipeline = gst_parse_launch(pipeline_str, &error);

    if (error) { 
        g_printerr("Pipeline error: %s\n", error->message); 
        return -1; 
    }

    /* Link the extraction probe to the parser's sink pad */
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
    
    g_print("\nExtractor exiting.\n");
    return 0;
}
