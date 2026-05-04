SUMMARY = "ICM20948 I2C Driver"
DESCRIPTION = "Custom ICM20948 accelerometer driver"

LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = "file://icm20948.c \
           file://icm20948_ioctl.h \
           file://Makefile"

S = "${WORKDIR}"

EXTRA_OEMAKE += "KERNEL_SRC=${STAGING_KERNEL_DIR}"
