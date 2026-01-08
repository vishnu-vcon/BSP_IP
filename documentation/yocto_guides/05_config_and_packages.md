# Configuration and Package Selection

This stage is about choosing the "Ingredients" for your Smart IP Camera distribution.

## 1. Global Configuration (`local.conf`)

In `build/conf/local.conf`, adjust these for a production-ready camera:

```bitbake
# The Board name from your BSP
MACHINE = "smartcam-v1"

# Use systemd for faster/organized daemon management
DISTRO_FEATURES:append = " systemd"
VIRTUAL-RUNTIME_init_manager = "systemd"

# Remove splash screen for faster boot
IMAGE_FEATURES:remove = "splash"

# Accept End User License Agreements (EULA) automatically for vendor layers
ACCEPT_FSL_EULA = "1"
```

## 2. Choosing Packages for an IP Camera

You don't want a "General Linux." You want a "Camera Linux." Here are the essential packages:

| Category | Package | Purpose |
| :--- | :--- | :--- |
| **Streaming** | `gstreamer1.0` | High-performance media framework. |
| **Vision** | `opencv` | (Optional) For AI/Object detection on the edge. |
| **Networking** | `dropbear` | A lightweight SSH server (replaces OpenSSH). |
| **System** | `busybox` | Minimalist versions of common Linux tools. |
| **Camera** | `v4l-utils` | Tools for testing the camera sensor (`v4l2-ctl`). |

## 3. Creating a Custom Image Recipe

Instead of building `core-image-minimal`, create your own: `recipes-core/images/smartcam-image.bb`.

```bitbake
SUMMARY = "Production image for Smart IP Camera"
LICENSE = "MIT"

inherit core-image

# Add your custom packages here
IMAGE_INSTALL:append = " \
    packagegroup-core-boot \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    camera-streamer \
    u-boot-fw-utils \
"

# Set a root password (example)
EXTRA_USERS_PARAMS = "usermod -p $(openssl passwd -1 mypassword) root;"
```

## 4. How to search for packages?

If you are looking for a specific software, use the **OpenEmbedded Layer Index**:
[layers.openembedded.org](https://layers.openembedded.org/)

This site tells you which layer contains the recipe you need. For example, if you need an RTSP server, search for `rtsp`.

---
> [!TIP]
> Use `IMAGE_FEATURES += "read-only-rootfs"` to protect your camera's OS from being corrupted if the power is pulled suddenly (very common for IP cameras!).
