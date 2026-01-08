# Beginner's Guide to Distro Development

Developing a custom distribution (Distro) for your IP camera involves a standardized workflow. Here is how you get from a clean PC to a booting camera.

## 1. Setting Up Your Host PC

Yocto requires a Linux host (Ubuntu 22.04 or 24.04 is recommended). You need about 100GB of free space and at least 8GB of RAM (16GB+ preferred).

### Install Dependencies
```bash
sudo apt-get install gawk wget git diffstat unzip texinfo gcc build-essential \
chrpath socat cpio python3 python3-pip python3-pexpect xz-utils debianutils \
iputils-ping python3-git python3-jinja2 python3-subunit zstd liblz4-tool \
file locales libacl1
```

## 2. Getting the Source (Poky)

The reference build system is called **Poky**.
```bash
git clone git://git.yoctoproject.org/poky
cd poky
git checkout scarthgap # Or the latest LTS version
```

## 3. The Development Workflow

### Step A: Initialize the Environment
Every time you open a new terminal to work on Yocto, you must "source" the environment:
```bash
source oe-init-build-env
```
This creates a `build/` directory and drops you inside it.

### Step B: Configure the Build
You modify two main files in `build/conf/`:
1. `local.conf`: Define your target hardware (`MACHINE`) and other global settings.
2. `bblayers.conf`: List the layers BitBake should look into.

### Step C: Add Layers
For a smart camera, you will likely need:
- `meta-openembedded`: For common utilities.
- `meta-vendor`: (e.g., `meta-rockchip`) provided by your hardware vendor.
- `meta-camera`: Your custom layer for your specific apps.

```bash
bitbake-layers add-layer ../meta-custom-layer
```

### Step D: Execute the Build
To build a basic image (like a minimal console system):
```bash
bitbake core-image-minimal
```
*Note: The first build can take 1-4 hours depending on your PC.*

### Step E: Flash and Test
The output will be in `tmp/deploy/images/<machine>/`. You take the `.wic` or `.img` file and write it to an SD card or eMMC.

## 4. Why "Create your own Distro"?

Creating a distribution (e.g., `meta-camera-os`) allows you to define:
- **Default Init System**: Use `systemd` for modern features or `sysvinit` for faster boot.
- **Global Optimization**: (e.g., `DEBUG_BUILD = "0"`).
- **Branding**: Customize the Linux kernel names and version strings.

---
> [!IMPORTANT]
> For a smart camera, your "Distro" configuration will likely focus on **Streaming Performance** and **Security**.
