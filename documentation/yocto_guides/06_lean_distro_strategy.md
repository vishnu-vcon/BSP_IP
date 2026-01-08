# Tailoring Your Distribution: The Lean IP Camera

For a smart camera, "Less is More." Stripping out unnecessary components like ALSA (audio) and USB support reduces boot time, power consumption, and security attack surfaces.

---

## 1. How to "Remove" things in Yocto

In Yocto, you generally don't delete files from other people's layers. Instead, you use **Policy Overrides** in your own layer or `local.conf`.

### A. Removing Global Features (`DISTRO_FEATURES`)
If your camera will never have a speaker or a USB port for peripherals, you should remove these from the global distribution features.

**Add this to your `conf/distro/camera-distro.conf` (or `local.conf` for testing):**
```bitbake
# Global feature removals
DISTRO_FEATURES:remove = "alsa pulseaudio bluetooth pcmcia 3g nfc"

# If you don't need USB host support (only power/debug)
DISTRO_FEATURES:remove = "usbhost"
```

### B. Excluding Specific Packages (`PACKAGE_EXCLUDE`)
If a vendor layer explicitly forces a package you don't want:
```bitbake
PACKAGE_EXCLUDE = "alsa-utils usbutils"
```

### C. Slimming the Kernel
Removing USB/Audio from the Kernel requires a **Kernel Fragment**. 

1.  Create `recipes-kernel/linux/files/lean-camera.cfg`.
2.  Add:
    ```bash
    # CONFIG_SOUND is not set
    # CONFIG_USB_SUPPORT is not set
    ```
3.  Apply it in your `.bbappend`:
    ```bitbake
    SRC_URI += "file://lean-camera.cfg"
    ```

---

## 2. How to "Add" Custom Software

To keep your design clean, keep your hardware support (BSP) and your applications (Software) in separate layers.

### Step 1: Create a Software Layer
```bash
bitbake-layers create-layer ../meta-camera-apps
```

### Step 2: Organize your Apps
Create recipes for your custom logic:
*   `recipes-apps/camera-ai/`: Your edge detection logic.
*   `recipes-apps/camera-service/`: Your proprietary daemon.

### Step 3: Create a Custom Image
The best way to ensure *only* what you want is in the build is to create your own image recipe that specifies exactly what to include.

**`recipes-core/images/camera-prod-image.bb`**:
```bitbake
SUMMARY = "Minimal Lean Production Image for Smart Camera"
inherit core-image

# 1. Base Essentials
IMAGE_INSTALL = "packagegroup-core-boot"

# 2. Add your custom apps
IMAGE_INSTALL += " \
    camera-ai \
    camera-service \
    gstreamer1.0-plugins-imx \
"

# 3. Explicitly avoid standard features you don't want
IMAGE_FEATURES = "ssh-server-dropbear"
IMAGE_FEATURES:remove = "splash"
```

---

## 3. The "Customization Workflow" Summary

1.  **BSP Layer (`meta-camera-bsp`)**: Handles the CPU architecture and specific drivers (i.MX 8MP + Sensor).
2.  **Software Layer (`meta-camera-apps`)**: Contains your proprietary `.bb` recipes.
3.  **Image Recipe**: Sets the final "Grocery List" of what goes onto the SD card.

> [!TIP]
> Use `IMAGE_FEATURES:remove` and `DISTRO_FEATURES:remove` first. They are cleaner than trying to exclude individual packages one-by-one.
