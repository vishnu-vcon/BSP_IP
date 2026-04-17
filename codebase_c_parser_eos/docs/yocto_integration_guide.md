# SMART IP Edge Application: Full Yocto Integration & Testing Guide

This guide details exactly **where** to put your code inside the Yocto BSP, **what** files to create, and **why** you do it, followed by the lightning-fast process for testing your code without reflashing the board.

---

## Phase 1: Adding the App to the Yocto Build System

You must create a custom "layer" in Yocto. Yocto searches all layers for `.bb` (recipe) files. A layer is just a folder structure that tells Yocto how to compile your C code.

We assume your Yocto project is located at `/media/shyam/Data/imx-yocto-bsp` and your active build folder is `full-image`.

### 1. Create the Custom Layer
**Where:** Inside your Yocto `sources/` directory.
**What to run:**
```bash
cd /media/shyam/Data/imx-yocto-bsp/sources
mkdir -p meta-smartip/conf
mkdir -p meta-smartip/recipes-apps/smartip-edge
```
**Why:** `meta-smartip` is your private layer. Yocto needs a `conf/` folder to recognize it as a layer, and `recipes-apps/` is the standard place to put user-space C applications.

### 2. Configure the Layer
**Where:** `/media/shyam/Data/imx-yocto-bsp/sources/meta-smartip/conf/layer.conf`
**What to add:** Create the `layer.conf` file with this exact content:
```bitbake
BBPATH .= ":${LAYERDIR}"
BBFILES += "${LAYERDIR}/recipes-*/*/*.bb"
BBFILE_COLLECTIONS += "meta-smartip"
BBFILE_PATTERN_meta-smartip = "^${LAYERDIR}/"
BBFILE_PRIORITY_meta-smartip = "6"
LAYERSERIES_COMPAT_meta-smartip = "walnascar scarthgap"
```
**Why:** This tells Yocto "Scan this directory for any `.bb` recipe files and compile them."

### 3. Tell Yocto to look at this layer
**Where:** `/media/shyam/Data/imx-yocto-bsp/full-image/conf/bblayers.conf`
**What to run:**
```bash
cd /media/shyam/Data/imx-yocto-bsp/full-image
bitbake-layers add-layer ../sources/meta-smartip
```
**Why:** Yocto ignores layers unless they are explicitly activated in the build configuration.

---

## Phase 2: Writing the Recipe

The recipe is the blueprint that finds your `codebase_c` folder, grabs the dependencies, runs Meson, and produces the `.exe`.

**Where:** `/media/shyam/Data/imx-yocto-bsp/sources/meta-smartip/recipes-apps/smartip-edge/smartip-edge_2.0.bb`
**What to add:** Create the file and paste this:

```bitbake
SUMMARY = "SMART IP Edge Video Application (C Port)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Tell Yocto to skip downloading and just use this exact physical folder
inherit externalsrc
EXTERNALSRC = "/media/shyam/Data/codebase_c"

# WHAT WE NEED TO COMPILE: The C dependencies (build-time headers + libs)
DEPENDS = " \
    glib-2.0 \
    json-glib \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-rtsp-server \
    libsoup-2.4 \
    zeromq \
    cairo \
    sqlite3 \
    openssl \
    qrencode \
    libpng \
    curl \
"

# BUILD SYSTEM: Triggers `meson setup` and `ninja`
inherit meson pkgconfig

# WHAT WE NEED TO RUN: Ensures the board rootfs contains these at runtime
RDEPENDS:${PN} += " \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-good-udp \
    gstreamer1.0-plugins-good-multifile \
    gstreamer1.0-plugins-good-isomp4 \
    gstreamer1.0-plugins-bad-mpegtsmux \
    gstreamer1.0-plugins-bad-jpegformat \
    gstreamer1.0-plugins-bad-shm \
    gstreamer1.0-plugins-good-video4linux2 \
    libgstapp-1.0 \
    imx-gst1.0-plugin \
    python3 \
    python3-numpy \
    python3-opencv \
    python3-zmq \
    python3-dbus \
    python3-pygobject \
    python3-json \
    python3-logging \
    python3-threading \
    tensorflow-lite \
"
```
**Why do this:** When Yocto reads this file, it knows: "Go to `/home/admin1/.../codebase_c`, make sure `gstreamer1.0-dev` is ready, run `meson`, and output the bin."

---

## Phase 3: The Fast Testing Workflow (Your Daily Loop)

You **DO NOT** need to rebuild the whole SD card image during development. Follow this loop to test C code changes in 10 seconds.

### Step 1: Tell Yocto to re-compile your app
Whenever you save a change to a `.c` file in your `codebase_c` folder on your PC, run this command in your Yocto build directory:
```bash
# Force Yocto to fetch the latest code and compile the app only
bitbake -c cleansstate smartip-edge
bitbake smartip-edge
```
*(This takes ~10 seconds. All your missing dependencies are provided natively by Yocto on your PC!)*

### Step 2: Extract the compiled binary
Yocto hides the finished executables inside its temporary workspace.
**Where the binary is created on your PC:**
`/media/shyam/Data/imx-yocto-bsp/full-image/tmp/work/armv8a-poky-linux/smartip-edge/2.0/packages-split/smartip-edge/usr/bin/smartip_engine`

### Step 3: Copy (SCP) the binary directly to the board
Upload the freshly compiled binary via SSH straight into the board’s root directory:
```bash
scp tmp/work/armv8a-poky-linux/smartip-edge/2.0/packages-split/smartip-edge/usr/bin/smartip_* root@192.168.1.160:/usr/bin/
```

### Step 4: Run it!
On the board's SSH terminal:
```bash
smartip_engine
```
If there is a bug, fix the C code, run `bitbake smartip-edge`, `scp` it over, and run it again.

---

## Phase 4: Final Production Image (MUST do this once!)

Because your custom C application relies on shared libraries like `libsoup`, `libzmq`, and `json-glib`, the physical SD card in your board **must** have these installed. You must run this flow at least once to bake these `.so` files onto your SD card. After you've done this once, you never have to do it again and can just use the Fast Testing Workflow!

When you are ready to build the full SD card image containing everything:

1. **Where:** `/media/shyam/Data/imx-yocto-bsp/full-image/conf/local.conf`
2. **What to add:** Append this line exactly:
   ```bitbake
   IMAGE_INSTALL:append = " smartip-edge"
   ```
   **Why:** This tells Yocto to inject your compiled application into the final `.wic.zst` filesystem.
3. **What to run:**
   ```bash
   bitbake imx-image-multimedia
   ```
4. **Locate the final image:**
   Yocto outputs the `.wic.zst` image file into the deploy directory. Find it here:
   ```bash
   ls -lh /media/shyam/Data/imx-yocto-bsp/full-image/tmp/deploy/images/imx8mpevk/*.wic.zst
   ```

5. **Flash it to your SD card:**
   Use `bmaptool` (highly recommended for speed) or `uuu` to flash the SD card:
   ```bash
   sudo bmaptool copy /media/shyam/Data/imx-yocto-bsp/full-image/tmp/deploy/images/imx8mpevk/imx-image-multimedia-imx8mpevk.wic.zst /dev/sdX
   ```
   *(Replace `/dev/sdX` with your actual SD card reader, like `/dev/mmcblk0` or `/dev/sdb`)*

---

## Appendix: How Yocto Actually Works (Conceptual Breakdown)

### 1. How does Yocto know where to find `smartip-edge`?
When you type `IMAGE_INSTALL:append = " smartip-edge"`, Yocto searches by following a strict **Breadcrumb Trail**:
1. **`local.conf`**: Says *"Find smartip-edge!"*
2. **`bblayers.conf`**: Says *"You are authorized to search inside `sources/meta-smartip/`"*
3. **`layer.conf`**: Says *"Scan all `recipes-*/` folders for `.bb` files!"*
4. Yocto searches and finds `smartip-edge_2.0.bb`. BINGO! It stops searching and uses it.

### 2. Where did the name `smartip-edge` come from?
The name has absolutely nothing to do with the layer name (`meta-smartip`). The name comes strictly from the **Filename of the Recipe**.
Yocto splits `smartip-edge_2.0.bb` into two variables:
- **`${PN}` (Package Name):** Everything *before* the underscore (`smartip-edge`).
- **`${PV}` (Package Version):** Everything *after* the underscore (`2.0`).
This is why `IMAGE_INSTALL` uses `smartip-edge`.

### 3. How do `DEPENDS` and `RDEPENDS` work?
- **`DEPENDS`**: Tells Yocto *"I need these C headers (like `<json-glib.h>`) to compile my code."* Yocto downloads them to your PC during `do_compile`.
- **`RDEPENDS`**: Tells Yocto *"If someone installs my app onto the SD card, you MUST physically copy `libsoup.so` and `json-glib.so` onto the SD card too."*
You never have to type `libsoup` or `gstreamer` manually into `local.conf`. Yocto automatically traces the `RDEPENDS` tree and installs all required background libraries into the final image!
