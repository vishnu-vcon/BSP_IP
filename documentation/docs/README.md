# Yocto Project Deep Dive: IP Camera Edition

Welcome to the comprehensive documentation suite for developing your smart IP camera Board Support Package (BSP). 

This repository contains a structured, beginner-friendly deep dive into every aspect of the Yocto Project.

## 📚 Documentation Modules

### [01. Foundations](file:///home/testuser/BSP/docs/01_Foundations/concepts_and_overview.md)
*   The Layer Model philosophy.
*   Architectural overview (BitBake, OE-Core, Poky).
*   The high-level build workflow.

### [02. BitBake Mastery](file:///home/testuser/BSP/docs/02_BitBake/syntax_and_operators.md)
*   Variable assignment operators (`=`, `?=`, `+=`, etc.).
*   Overrides and conditional logic.
*   Inline Python in metadata.

### [03. Layers & Recipes](file:///home/testuser/BSP/docs/03_Layers_and_Recipes/writing_recipes.md)
*   Recipe structure and task execution flow.
*   Inheriting classes (CMake, Autotools).
*   The `do_install` task in detail.

### [04. BSP & Hardware Integration](file:///home/testuser/BSP/docs/04_BSP_and_Hardware/machine_configurations.md)
*   Writing machine configuration files (`.conf`).
*   **[BSP vs. BSP Layer](file:///home/testuser/BSP/docs/04_BSP_and_Hardware/bsp_vs_bsp_layer.md)**: Clearing the confusion.
*   **[Kernel Customization & Drivers](file:///home/testuser/BSP/docs/04_BSP_and_Hardware/kernel_customization.md)**: Handling sensors and configuration fragments.


### [05. Building Your Distro](file:///home/testuser/BSP/docs/05_Images_and_Distros/image_customization.md)
*   Customizing images with `IMAGE_INSTALL`.
*   Global configurations in `local.conf`.
*   Handling commercial licenses (codecs) and OTA updates.

### [06. Application Development](file:///home/testuser/BSP/docs/06_SDK_and_Apps/sdk_usage.md)
*   Generating and using the SDK.
*   Using `devtool` for rapid application development.
*   IDE integration and remote debugging.

---

## 🚀 Quick Start for IP Camera
1.  **Clone Poky**: `git clone git://git.yoctoproject.org/poky`
2.  **Initialize**: `source oe-init-build-env`
3.  **Create your BSP layer**: `bitbake-layers create-layer ../meta-ipc-bsp`
4.  **Define your Machine**: Edit `meta-ipc-bsp/conf/machine/smart-camera.conf`.
5.  **Build**: `bitbake core-image-minimal`

---

## 🛠 Useful Commands
| Command | Purpose |
| :--- | :--- |
| `bitbake-layers show-layers` | See all active layers. |
| `bitbake -e <recipe>` | View the fully expanded metadata for a recipe. |
| `bitbake -c listtasks <recipe>` | See all tasks available for a recipe. |
| `devtool modify <recipe>` | Enter development mode for a package. |
