# 05. Kernel Development: Customization & Drivers

Developing an IP camera usually involves custom kernel configurations or patches for image sensors (CMOS).

## 1. How Yocto Manages the Kernel

Unlike a standard recipe, the kernel is complex. Yocto uses **Configuration Fragments** (`.cfg`) to manage it modularly.

### 1.1 The `.bbappend` Workflow
To modify the kernel without copying the whole recipe:
1. Create `recipes-kernel/linux/linux-yocto_%.bbappend`
2. Add your changes.

```python
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://0001-add-camera-sensor-support.patch \
            file://camera-tweak.cfg"
```

---

## 2. Configuration Fragments (`.cfg`)

A fragment is just a list of `CONFIG_` options that you want to change. You don't need the whole `.config` file.

**`camera-tweak.cfg`**:
```text
CONFIG_VIDEO_IMX219=y
CONFIG_I2C_DEBUG_BUS=y
CONFIG_V4L2_MEM2MEM_DEV=y
```

BitBake will automatically merge these fragments into the final kernel configuration.

---

## 3. Applying Patches

If you have a driver from a vendor in `.patch` format:
1. Place it in the `files` directory next to your recipe.
2. Add it to `SRC_URI`.
3. Yocto will automatically run `patch -p1` during the `do_patch` task.

---

## 4. Using `menuconfig` the "Yocto Way"

Instead of running `make menuconfig` manually and losing changes:
1. Run: `bitbake -c menuconfig virtual/kernel`
2. Make your changes in the GUI and Save.
3. Run: `bitbake -c diffconfig virtual/kernel`
4. This will output a **fragment file**. Copy this fragment into your layer!

---

## 5. Adding Out-of-Tree Modules

Sometimes drivers are provided as separate source code (not part of the kernel tree).

Inherit the `module` class in your recipe:
```python
SUMMARY = "Custom IP Camera Sensor Driver"
LICENSE = "GPLv2"
inherit module

SRC_URI = "file://my-driver-source/"
S = "${WORKDIR}/my-driver-source"

# The module class automatically handles compiling against the current kernel
```

---

## 6. Device Trees (DTB)

The Device Tree describes the hardware pins (I2C, MIPI CSI, GPIO).
For a camera sensor:
1. Define the sensor node under the correct I2C bus.
2. Link the sensor port to the SoC's CSI port using "endpoints".

> [!WARNING]
> If your kernel isn't booting or your camera isn't found on `/dev/video0`, the Device Tree is the first place you should check!
