# Writing a Board Support Package (BSP) Layer

A BSP layer connects the Yocto Project to your specific hardware (the Smart IP Camera CPU/Board).

## 1. BSP Layer Structure

A typical BSP layer (e.g., `meta-camera-bsp`) looks like this:
```text
meta-camera-bsp/
├── conf/
│   ├── layer.conf
│   └── machine/
│       └── smartcam-v1.conf   <-- Defines your board
├── recipes-bsp/
│   └── u-boot/                <-- Bootloader patches
├── recipes-kernel/
│   └── linux/                 <-- Kernel recipe and config
```

## 2. The Machine Configuration File

In `conf/machine/smartcam-v1.conf`, you define what the hardware is:

```bitbake
# The CPU architecture
TARGET_ARCH = "arm"
DEFAULTTUNE = "cortexa7hf-neon-vfpv4"

# The Kernel to use
PREFERRED_PROVIDER_virtual/kernel = "linux-yocto-custom"
KERNEL_IMAGETYPE = "zImage"
KERNEL_DEVICETREE = "smartcam-v1.dtb"

# The Bootloader
PREFERRED_PROVIDER_virtual/bootloader = "u-boot"
UBOOT_MACHINE = "smartcam_defconfig"

# Image features
IMAGE_FSTYPES = "tar.bz2 wic.gz"
```

## 3. Kernel and Device Tree

The Device Tree (DTS) is the bridge between the hardware and the software. For an IP camera, you will typically need to enable:
- **MIPI CSI**: For the camera sensor input.
- **I2C**: For controlling the sensor.
- **Ethernet/WiFi**: For streaming.

### How to add a Kernel Patch
If your sensor vendor gives you a `.patch` file:
1. Put it in `recipes-kernel/linux/files/`.
2. Create a `.bbappend` file for your kernel.

```bitbake
# recipes-kernel/linux/linux-yocto-custom_%.bbappend
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://0001-add-sensor-driver.patch"
```

## 4. Handling Vendor SDKs

Many IP camera chips (like those from HiSilicon or Rockchip) come with "Binary Blobs" (SDKs).
- **Pro Tip**: Create a `meta-vendor` layer.
- Place the `.so` libraries and headers in a recipe using `do_install`.
- Use `INSANE_SKIP` if Yocto complains about binary-only files.

---
> [!IMPORTANT]
> A BSP developer's main job is ensuring the **Kernel** can see all hardware and the **Bootloader** knows how to load the Kernel.
