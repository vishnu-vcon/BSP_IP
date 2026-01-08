# Creating Meta Layers and Recipes

This is where you write your own code and tell Yocto how to compile it.

## 1. Creating a Meta Layer

A layer is just a directory with a specific internal structure. Use the provided tool to create one:

```bash
cd poky
bitbake-layers create-layer ../meta-camera-apps
```

### Layer Structure
- `conf/layer.conf`: Tells BitBake this is a layer and defines its priority.
- `recipes-example/`: A template recipe directory.

## 2. Creating a Recipe (`.bb` file)

A recipe contains instructions for BitBake. Let’s say you have a custom RTSP streaming app written in C.

### Example Recipe: `recipes-apps/camera-streamer/camera-streamer_1.0.bb`

```bitbake
SUMMARY = "Custom RTSP Streamer for Smart IP Camera"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Where is the code?
SRC_URI = "git://github.com/mycompany/camera-streamer.git;protocol=https;branch=main"
SRCREV = "abcdef123456" # Specific Git hash

# Specify dependencies
DEPENDS = "gstreamer1.0 gstreamer1.0-plugins-base"

# Work directory
S = "${WORKDIR}/git"

# How to build it (e.g., using CMake)
inherit cmake

# Optional: Add files to the image
do_install() {
    install -d ${D}${bindir}
    install -m 0755 camera-streamer ${D}${bindir}
}
```

## 3. Key Recipe Variables

| Variable | Meaning |
| :--- | :--- |
| `SUMMARY` | Short description of the package. |
| `LICENSE` | The software license (e.g., MIT, GPLv2). |
| `SRC_URI` | The location of the source code (Git, HTTP, local files). |
| `DEPENDS` | Build-time dependencies (what needs to be compiled first). |
| `RDEPENDS` | Runtime dependencies (what needs to be on the SD card). |
| `inherit` | Uses a class to simplify the build (e.g., `autotools`, `cmake`, `python_setuptools3`). |

## 4. Helpful BitBake Commands

- **`bitbake -c listtasks <recipe>`**: See all available tasks (fetch, compile, install).
- **`bitbake <recipe>`**: Build just this recipe.
- **`bitbake -c devshell <recipe>`**: Open a terminal inside the build environment (great for debugging!).

---
> [!TIP]
> Always use `inherit cmake` or `inherit autotools` if your software uses those build systems. Yocto handles the cross-compilation magic for you.
