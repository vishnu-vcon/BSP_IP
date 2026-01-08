# 04. BSP Development: Machine Configuration

The Machine Configuration (`.conf`) is the heart of a Board Support Package. It lives in `meta-your-layer/conf/machine/machine-name.conf`.

> [!NOTE]
> New to BSPs? Check out the **[BSP vs. BSP Layer](file:///home/testuser/BSP/docs/04_BSP_and_Hardware/bsp_vs_bsp_layer.md)** guide to understand how hardware support is organized.


## 1. What belongs in a Machine Conf?

A machine file should only contain information about the **physical hardware**. Software policy belongs in a Distro file.

### 1.1 CPU Tuning
This determines how the code is compiled for your CPU (e.g., Neon support, Hard-float).

```python
# Include standard tuning files from OE-Core
include conf/machine/include/arm/armv7a/tune-cortexa7.inc
```

### 1.2 Kernel Selection
```python
# Tell Yocto which kernel recipe to use
PREFERRED_PROVIDER_virtual/kernel = "linux-vendor"
# Set the version
PREFERRED_VERSION_linux-vendor = "5.10%"
# Define the image format (zImage, uImage, fitImage)
KERNEL_IMAGETYPE = "zImage"
# Device tree files to build
KERNEL_DEVICETREE = "vendor/my-camera-board.dtb"
```

### 1.3 Bootloader Selection
```python
PREFERRED_PROVIDER_virtual/bootloader = "u-boot"
UBOOT_MACHINE = "my_board_defconfig"
```

---

## 2. Important Machine Variables

| Variable | Usage |
| :--- | :--- |
| `MACHINE_FEATURES` | Hardware capabilities: `wifi`, `bluetooth`, `rtc`, `usbhost`, `screen`. |
| `MACHINE_ESSENTIAL_EXTRA_RDEPENDS` | Packages required to boot (e.g., specific firmware). |
| `SERIAL_CONSOLES` | Defines the debug UART: `"115200;ttyS0"`. |
| `IMAGE_FSTYPES` | The output format: `wic`, `tar.bz2`, `ext4`. |

---

## 3. Handling Hardware Specializations

### Graphics/Video (Crucial for IP Camera)
If your SoC has a dedicated ISP or Video Encoder (VPU), you need to include the drivers here.

```python
# Add VPU firmware as a requirement for booting
MACHINE_ESSENTIAL_EXTRA_RDEPENDS += "vpu-firmware-vendor"

# Recommend additional support packages
MACHINE_EXTRA_RRECOMMENDS += "kernel-module-isp-driver"
```

---

## 4. Best Practices for BSPs

1.  **Use `?=` for assignments**: This allows users to override your machine settings in their `local.conf` if they need to.
2.  **Inherit standard includes**: Don't reinvent the wheel for standard CPU architectures.
3.  **Include a README**: Tell people how to flash the image once it's built!

> [!TIP]
> Use the `wic` tool for image creation. It allows you to define partitions (Boot, RootFS, UserData) in a `.wks` file, making it easy to create a "single file" to flash onto an SD card.
