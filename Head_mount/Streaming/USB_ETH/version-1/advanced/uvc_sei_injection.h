#ifndef UVC_SEI_INJECTION_H
#define UVC_SEI_INJECTION_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <gst/gst.h>

#include "sei_metadata.h"
#include "icm20948_ioctl.h"

/* ─── Application context (no threads, no mutex) ───────────────────────── */
typedef struct {
    int imu_fd;                    /* /dev/icm20948 fd, or -1 if unavailable */
    uint32_t frame_counter;        /* Monotonic frame ID */
    /* Pre-filled static metadata */
    char     device_serial[16];
    uint8_t  environment_type;
    int32_t  latitude_e7;
    int32_t  longitude_e7;
    char     session_id[16];
} AppContext;

static AppContext app_ctx = {
    .imu_fd = -1,
    .frame_counter = 0,
    .device_serial = "IMX8MP-001",
    .environment_type = SEI_ENV_INDOOR,
    .latitude_e7 = 129716000,       /* 12.9716° N */
    .longitude_e7 = 775946000,      /* 77.5946° E */
    .session_id = "session-0001",
};

/* ─── IMU device open (called once at startup) ─────────────────────────── */
static int imu_open(void)
{
    app_ctx.imu_fd = open("/dev/icm20948", O_RDONLY);
    if (app_ctx.imu_fd < 0) {
        printf("IMU: /dev/icm20948 not found — using simulated values.\n");
        return -1;
    }
    printf("IMU: /dev/icm20948 opened (fd=%d) — real sensor data.\n", app_ctx.imu_fd);
    return 0;
}

/* ─── Read IMU inline (called per-frame in pad probe) ──────────────────── */
static void read_imu_data(SeiMetadata *meta)
{
    if (app_ctx.imu_fd >= 0) {
        /* Real sensor: single IOCTL, ~200-360µs */
        struct icm20948_all_data imu;
        if (ioctl(app_ctx.imu_fd, ICM20948_IOC_GET_ALL, &imu) == 0) {
            meta->accel_x_mg  = imu.accel_x_mg;
            meta->accel_y_mg  = imu.accel_y_mg;
            meta->accel_z_mg  = imu.accel_z_mg;
            meta->gyro_x_mdps = imu.gyro_x_mdps;
            meta->gyro_y_mdps = imu.gyro_y_mdps;
            meta->gyro_z_mdps = imu.gyro_z_mdps;
            meta->mag_x_ut    = imu.mag_x_ut;
            meta->mag_y_ut    = imu.mag_y_ut;
            meta->mag_z_ut    = imu.mag_z_ut;
            return;
        }
    }
    /* Fallback: simulated values */
    static int sim_counter = 0;
    sim_counter++;
    meta->accel_x_mg  = 10 + (sim_counter % 50);
    meta->accel_y_mg  = -980 + (sim_counter % 20);
    meta->accel_z_mg  = 45 + (sim_counter % 30);
    meta->gyro_x_mdps = 150 + (sim_counter % 100);
    meta->gyro_y_mdps = -20 + (sim_counter % 40);
    meta->gyro_z_mdps = 5 + (sim_counter % 10);
    meta->mag_x_ut    = 25 + (sim_counter % 15);
    meta->mag_y_ut    = -12 + (sim_counter % 8);
    meta->mag_z_ut    = 40 + (sim_counter % 20);
}

/* ─── Emulation Prevention Bytes ───────────────────────────────────────── */
static int apply_emulation_prevention(const uint8_t *src, int src_len, uint8_t *dst)
{
    int dst_len = 0, zero_count = 0;
    for (int i = 0; i < src_len; i++) {
        if (zero_count == 2 && src[i] <= 0x03) {
            dst[dst_len++] = 0x03;
            zero_count = 0;
        }
        if (src[i] == 0x00) zero_count++;
        else                zero_count = 0;
        dst[dst_len++] = src[i];
    }
    return dst_len;
}

/* ─── Circular IMU History Buffer ──────────────────────────────────────── *
 *
 * Threading model (no queue in pipeline):
 *   v4l2src:src → [camera_src_probe WRITES] → videoconvert → v4l2h264enc
 *   → [sei_injection_probe READS] → h264parse → rtph264pay
 *
 * All probes fire on the SAME streaming thread, in chain-function order.
 * The write always completes before the read for the same PTS.
 *
 * If a queue is added (recommended for robustness), ordering is still
 * preserved: the buffer must physically pass through the queue (which
 * uses an internal mutex) before reaching the encoder's src pad.
 *
 * write_seq tracks overwrites: if the reader detects that write_seq has
 * advanced by more than MAX_IMU_HISTORY, entries were lost.
 */
#define MAX_IMU_HISTORY 64

typedef struct {
    GstClockTime pts;
    uint32_t     write_seq;        /* Writer's sequence number at time of write */
    SeiMetadata  meta;
} ImuHistoryEntry;

static ImuHistoryEntry imu_history[MAX_IMU_HISTORY];
static uint32_t imu_write_seq = 0;   /* Monotonic write counter */
static int imu_history_read_idx = 0;

/* ─── GStreamer pad probe: Camera Source (Zero-Latency Sampling) ──────── */
static GstPadProbeReturn camera_src_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    GstClockTime pts = GST_BUFFER_PTS(buffer);

    SeiMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic   = SEI_METADATA_MAGIC;
    meta.version = SEI_METADATA_VERSION;
    meta.frame_pts_ns = pts;

    /* Sample CLOCK_MONOTONIC exactly when frame hits user-space */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    meta.imu_timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    /* Per-frame UTC wall-clock timestamp */
    struct timespec utc;
    clock_gettime(CLOCK_REALTIME, &utc);
    meta.utc_timestamp_us = (uint64_t)utc.tv_sec * 1000000ULL
                          + (uint64_t)utc.tv_nsec / 1000ULL;

    meta.frame_id = app_ctx.frame_counter++;

    /* Read IMU data exactly at camera frame delivery (~200-360µs) */
    read_imu_data(&meta);

    /* Static embedded metadata */
    memcpy(meta.device_serial, app_ctx.device_serial, 16);
    meta.environment_type = app_ctx.environment_type;
    meta.latitude_e7      = app_ctx.latitude_e7;
    meta.longitude_e7     = app_ctx.longitude_e7;
    memcpy(meta.session_id, app_ctx.session_id, 16);

    /* O(1) Circular Buffer Store */
    uint32_t seq = imu_write_seq++;
    int idx = seq % MAX_IMU_HISTORY;
    imu_history[idx].pts       = pts;
    imu_history[idx].meta      = meta;
    imu_history[idx].write_seq = seq;  /* Written last so reader can detect torn entries */

    return GST_PAD_PROBE_OK;
}

/* ─── GStreamer pad probe: SEI injection (< 1ms total) ─────────────────── */
static GstPadProbeReturn sei_injection_probe(GstPad *pad, GstPadProbeInfo *info,
                                              gpointer user_data)
{
    (void)pad;

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    int is_hevc = GPOINTER_TO_INT(user_data);
    GstClockTime pts = GST_BUFFER_PTS(buffer);

    /* ── 1. Look up SeiMetadata from circular buffer ─────────────────── */
    SeiMetadata meta;
    int found = 0;

    /* Search from last known read position */
    int start_idx = imu_history_read_idx;
    for (int i = 0; i < MAX_IMU_HISTORY; i++) {
        int idx = (start_idx + i) % MAX_IMU_HISTORY;
        if (imu_history[idx].pts == pts && pts != GST_CLOCK_TIME_NONE) {
            /* Overflow detection: check if the entry is still valid */
            uint32_t age = imu_write_seq - imu_history[idx].write_seq;
            if (age > MAX_IMU_HISTORY) {
                /* Entry was overwritten — should not happen in normal operation */
                g_printerr("SEI: WARNING — circular buffer overflow detected "
                           "(age=%u, max=%d)\n", age, MAX_IMU_HISTORY);
                break;
            }
            meta = imu_history[idx].meta;
            found = 1;
            imu_history_read_idx = (idx + 1) % MAX_IMU_HISTORY;
            break;
        }
    }

    /* Fallback if PTS match failed (should not happen in single-thread pipeline) */
    if (!found) {
        memset(&meta, 0, sizeof(meta));
        meta.magic = SEI_METADATA_MAGIC;
        meta.version = SEI_METADATA_VERSION;
        meta.frame_pts_ns = pts;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        meta.imu_timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        struct timespec utc;
        clock_gettime(CLOCK_REALTIME, &utc);
        meta.utc_timestamp_us = (uint64_t)utc.tv_sec * 1000000ULL
                              + (uint64_t)utc.tv_nsec / 1000ULL;
        read_imu_data(&meta);
        g_printerr("SEI: WARNING — PTS lookup miss for PTS=%" G_GUINT64_FORMAT
                    ", using fallback IMU read\n", pts);
    }

    /* ── 2. Serialize: UUID(16) + SeiMetadata → raw RBSP ──────────────── */
    uint8_t uuid[16] = {0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7,
                        0x96, 0x2c, 0xd3, 0x9c, 0x24, 0x5a, 0xc8, 0x1a};
    
    int raw_size = 16 + sizeof(SeiMetadata);  /* RBSP payload size (per H.264 spec) */
    uint8_t *raw = g_malloc(raw_size);
    memcpy(raw, uuid, 16);
    memcpy(raw + 16, &meta, sizeof(SeiMetadata));

    /* Apply emulation prevention to get EBSP */
    uint8_t *epb = g_malloc(raw_size * 2);
    int epb_len = apply_emulation_prevention(raw, raw_size, epb);

    /* ── 3. Construct SEI NAL ──────────────────────────────────────────── *
     *
     * H.264 SEI payloadSize = RBSP size (before EPB) = raw_size
     * This is per ITU-T H.264 Annex D: payloadSize counts RBSP bytes,
     * NOT the EBSP bytes after emulation prevention insertion.
     * The decoder removes EPB first, then reads payloadSize bytes.
     */
    uint8_t sei_header[10];
    int header_len = 0;

    if (is_hevc) {
        sei_header[0] = 0x00; sei_header[1] = 0x00;
        sei_header[2] = 0x00; sei_header[3] = 0x01;
        sei_header[4] = 0x4E; sei_header[5] = 0x01;
        sei_header[6] = 0x05;
        header_len = 7;
    } else {
        sei_header[0] = 0x00; sei_header[1] = 0x00;
        sei_header[2] = 0x00; sei_header[3] = 0x01;
        sei_header[4] = 0x06; sei_header[5] = 0x05;
        header_len = 6;
    }

    /* Payload size encoding: raw_size (RBSP) in multi-byte format */
    uint8_t size_bytes[4];
    int size_len = 0;
    int remaining = raw_size;
    while (remaining >= 255) {
        size_bytes[size_len++] = 0xFF;
        remaining -= 255;
    }
    size_bytes[size_len++] = (uint8_t)remaining;

    int total = header_len + size_len + epb_len + 1; /* +1 for RBSP trailing */
    uint8_t *pkt = g_malloc(total);

    int off = 0;
    memcpy(pkt + off, sei_header, header_len); off += header_len;
    memcpy(pkt + off, size_bytes, size_len);   off += size_len;
    memcpy(pkt + off, epb, epb_len);           off += epb_len;
    pkt[off] = 0x80; /* RBSP trailing bits */

    /* ── 4. Prepend SEI to video buffer ────────────────────────────────── */
    GstBuffer *sei_buf = gst_buffer_new_wrapped(pkt, total);
    GstBuffer *new_buffer = gst_buffer_append(sei_buf, gst_buffer_ref(buffer));

    GST_PAD_PROBE_INFO_DATA(info) = new_buffer;
    gst_buffer_unref(buffer);

    g_free(raw);
    g_free(epb);

    return GST_PAD_PROBE_OK;
}

#endif /* UVC_SEI_INJECTION_H */
