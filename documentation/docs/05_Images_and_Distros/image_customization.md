# 06. Image Configuration: Building Your Distro

The final output of Yocto is an **Image**. Customizing what goes into that image is how you build your product's "personality".

## 1. Choosing Packages (`IMAGE_INSTALL`)

There are three ways to add packages to your image:

### 1.1 In `local.conf` (Development quick-fix)
```python
IMAGE_INSTALL:append = " gdb gstreamer1.0-plugins-bad"
```

### 1.2 In a Custom Image Recipe (Best for Production)
Create: `recipes-core/images/my-ipc-image.bb`
```python
inherit core-image
IMAGE_INSTALL += "packagegroup-core-boot my-app-1 my-app-2"
```

### 1.3 Using Package Groups (Modular approach)
A Package Group is a recipe that just lists other recipes. 
Example: `packagegroup-camera-stack`. This makes it easy to reuse the same "camera features" across multiple image types.

---

## 2. Image Features

Yocto provides "shortcuts" for common configurations.

```python
# Enable a debug console without needing a root password (Development only!)
EXTRA_IMAGE_FEATURES += "debug-tweaks"

# Include documentation for all packages
EXTRA_IMAGE_FEATURES += "doc-pkgs"

# Make the root filesystem read-only (Good for IP Cameras!)
IMAGE_FEATURES += "read-only-rootfs"

# Create an SSH server (OpenSSH)
IMAGE_FEATURES += "ssh-server-openssh"
```

---

## 3. Global Configuration (`local.conf`)

Important variables for every build:

- `DL_DIR`: Where to store downloaded source code (Share this across projects!).
- `SSTATE_DIR`: Where to store the build cache (Speeds up future builds).
- `TMPDIR`: Where the actual build happens (Needs lots of space).
- `SDKMACHINE`: If your team uses macOS or Windows (via WSL), set this appropriately.

---

## 4. Handling Commercial Licenses

Many media codecs (H.264, MPEG-2) require license fees. Yocto blocks these by default to protect you legally.

If you are using GStreamer/FFmpeg with these codecs, you must add:
```python
LICENSE_FLAGS_ACCEPTED = "commercial"
```

---

## 5. Over-the-Air (OTA) Updates

For a customer-facing device like an IP camera, you CANNOT expect users to flash an SD card manually.

**Popular Yocto-compatible OTA solutions:**
- **Mender.io**: Dual-partition A/B updates. Very reliable.
- **RAUC**: Lightweight, flexible, supports bundles.
- **SWUpdate**: Highly customizable, supports many storage types.

> [!TIP]
> Use `IMAGE_GEN_HSM_SIGN` variables if you need to sign your images for Secure Boot!
