/*
 * sei_metadata.h — Shared SEI Metadata Payload Definition
 *
 * Used by both the injection side (gadget/board) and the extraction side (host PC).
 * Transport-agnostic: works for UVC streaming, Ethernet streaming, or file recording.
 *
 * The struct is packed with fixed-width types to ensure identical binary layout
 * on ARM (i.MX8MP) and x86 (host PC).
 *
 * Copyright (C) 2026
 */

#ifndef SEI_METADATA_H
#define SEI_METADATA_H

#include <stdint.h>

/* ─── Magic and versioning ──────────────────────────────────────────────── */
#define SEI_METADATA_MAGIC    0x494D5538   /* ASCII "IMU8" */
#define SEI_METADATA_VERSION  1

/* ─── Environment type codes ────────────────────────────────────────────── */
#define SEI_ENV_INDOOR    0
#define SEI_ENV_OUTDOOR   1
#define SEI_ENV_VEHICLE   2
#define SEI_ENV_AERIAL    3

/* ─── SEI Metadata Payload Structure ────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    /* ── Header (5 bytes) ─────────────────────────────────────────────── */
    uint32_t magic;                  /* SEI_METADATA_MAGIC — validation tag */
    uint8_t  version;                /* SEI_METADATA_VERSION              */

    /* ── Timestamps (28 bytes) ────────────────────────────────────────── */
    uint64_t frame_pts_ns;           /* GST_BUFFER_PTS — camera capture time
                                      * (CLOCK_MONOTONIC, nanoseconds)   */
    uint64_t imu_timestamp_ns;       /* clock_gettime(CLOCK_MONOTONIC)
                                      * when IMU was read in pad probe   */
    uint64_t utc_timestamp_us;       /* CLOCK_REALTIME — wall-clock time
                                      * (microseconds since epoch)       */
    uint32_t frame_id;               /* Monotonic frame counter          */

    /* ── IMU: Accelerometer (12 bytes) ────────────────────────────────── */
    int32_t  accel_x_mg;             /* X-axis acceleration in milli-g   */
    int32_t  accel_y_mg;             /* Y-axis acceleration in milli-g   */
    int32_t  accel_z_mg;             /* Z-axis acceleration in milli-g   */

    /* ── IMU: Gyroscope (12 bytes) ────────────────────────────────────── */
    int32_t  gyro_x_mdps;            /* X-axis rotation in milli-dps     */
    int32_t  gyro_y_mdps;            /* Y-axis rotation in milli-dps     */
    int32_t  gyro_z_mdps;            /* Z-axis rotation in milli-dps     */

    /* ── IMU: Magnetometer (12 bytes) ─────────────────────────────────── */
    int32_t  mag_x_ut;               /* X-axis magnetic field in micro-T */
    int32_t  mag_y_ut;               /* Y-axis magnetic field in micro-T */
    int32_t  mag_z_ut;               /* Z-axis magnetic field in micro-T */

    /* ── Embedded Metadata (41 bytes) ─────────────────────────────────── */
    char     device_serial[16];      /* Device serial number (null-term) */
    uint8_t  environment_type;       /* SEI_ENV_* code                   */
    int32_t  latitude_e7;            /* Latitude  × 1e7  (integer)       */
    int32_t  longitude_e7;           /* Longitude × 1e7  (integer)       */
    char     session_id[16];         /* User/session ID   (null-term)    */

} SeiMetadata;
/* Total packed size: 5 + 28 + 12 + 12 + 12 + 41 = 110 bytes */

_Static_assert(sizeof(SeiMetadata) == 110,
               "SeiMetadata struct size mismatch — check packing");

#endif /* SEI_METADATA_H */
