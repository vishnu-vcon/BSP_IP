/*
 * icm20948_ioctl.h — Shared IOCTL definitions for ICM-20948 driver
 *
 * Used by both the kernel driver (icm20948.c) and userspace applications
 * (uvc_sei_injection.h) to share IOCTL command codes and data structures.
 *
 * Copyright (C) 2026
 */

#ifndef ICM20948_IOCTL_H
#define ICM20948_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <stdint.h>
typedef uint8_t  u8;
#endif

/* ─── IOCTL magic number ───────────────────────────────────────────────── */
#define ICM20948_IOC_MAGIC          'K'

/* ─── Accelerometer IOCTLs (existing) ──────────────────────────────────── */
#define ICM20948_IOC_RESET          _IO (ICM20948_IOC_MAGIC, 0x00)
#define ICM20948_IOC_GET_ACCEL_X    _IOR(ICM20948_IOC_MAGIC, 0x01, int)
#define ICM20948_IOC_GET_ACCEL_Y    _IOR(ICM20948_IOC_MAGIC, 0x02, int)
#define ICM20948_IOC_GET_ACCEL_Z    _IOR(ICM20948_IOC_MAGIC, 0x03, int)
#define ICM20948_IOC_SET_ACCEL_FSR  _IOW(ICM20948_IOC_MAGIC, 0x04, u8)
#define ICM20948_IOC_SET_ACCEL_ODR  _IOW(ICM20948_IOC_MAGIC, 0x05, u8)

/* ─── Gyroscope IOCTLs (new) ──────────────────────────────────────────── */
#define ICM20948_IOC_GET_GYRO_X     _IOR(ICM20948_IOC_MAGIC, 0x10, int)
#define ICM20948_IOC_GET_GYRO_Y     _IOR(ICM20948_IOC_MAGIC, 0x11, int)
#define ICM20948_IOC_GET_GYRO_Z     _IOR(ICM20948_IOC_MAGIC, 0x12, int)
#define ICM20948_IOC_SET_GYRO_FSR   _IOW(ICM20948_IOC_MAGIC, 0x13, u8)
#define ICM20948_IOC_SET_GYRO_ODR   _IOW(ICM20948_IOC_MAGIC, 0x14, u8)

/* ─── Magnetometer IOCTLs (new) ──────────────────────────────────────── */
#define ICM20948_IOC_GET_MAG_X      _IOR(ICM20948_IOC_MAGIC, 0x20, int)
#define ICM20948_IOC_GET_MAG_Y      _IOR(ICM20948_IOC_MAGIC, 0x21, int)
#define ICM20948_IOC_GET_MAG_Z      _IOR(ICM20948_IOC_MAGIC, 0x22, int)

/* ─── Bulk read: all 9 axes in one IOCTL (new) ────────────────────────── */
struct icm20948_all_data {
    int accel_x_mg,  accel_y_mg,  accel_z_mg;   /* milli-g           */
    int gyro_x_mdps, gyro_y_mdps, gyro_z_mdps;  /* milli-deg/sec     */
    int mag_x_ut,    mag_y_ut,    mag_z_ut;      /* micro-Tesla       */
};

#define ICM20948_IOC_GET_ALL        _IOR(ICM20948_IOC_MAGIC, 0x30, struct icm20948_all_data)

#endif /* ICM20948_IOCTL_H */
