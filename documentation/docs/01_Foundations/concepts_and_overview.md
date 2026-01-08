# 01. Yocto Project: Concepts & Overview

The Yocto Project is an open-source collaboration project that provides templates, tools, and methods to help you create custom Linux-based systems for embedded products, regardless of the hardware architecture.

## 1. Key Components

### 1.1 BitBake
The **engine** of the Yocto Project. It is a scheduler and execution engine that parses "recipes" (instructions) and executes tasks to build software packages.

### 1.2 OpenEmbedded-Core (OE-Core)
A set of foundational metadata (recipes, classes, and configuration files) that are common across many different projects. It defines how to build the base Linux system.

### 1.3 Poky
The **reference distribution**. It is a collection of Yocto Project tools (BitBake, OE-Core, etc.) and a specific configuration used to test and demonstrate the project's capabilities. It's the best starting point for most developers.

---

## 2. The Layer Model: The Core Philosophy

Yocto Project's most powerful feature is its **Layer Model**. Instead of modifying the core files, you add your changes in separate "layers".

| Layer Type | Purpose | Example |
| :--- | :--- | :--- |
| **Base Layer** | Foundation of the build system. | `meta` (OE-Core) |
| **Silicon Layer** | CPU/SoC specific support (drivers, tuning). | `meta-intel`, `meta-arm` |
| **BSP Layer** | Support for a specific physical board. | `meta-raspberrypi`, `meta-ipc-bsp` |
| **UI/App Layer** | Desktop environments, UI frameworks, or apps. | `meta-qt6`, `meta-ipc-apps` |
| **Distro Layer** | Global policy (systemd vs sysvinit, branding). | `meta-poky` |

### Why use layers?
1.  **Modularity**: Swap a hardware layer without changing your application code.
2.  **Upgradability**: Pull the latest updates for `meta-arm` without losing your custom configurations.
3.  **Collaboration**: Share layers with the community or vendors.

---

## 3. The Build Workflow (High Level)

1.  **User Configuration**: You define what you want to build (Machine, Distro, Selection of packages).
2.  **Metadata Parsing**: BitBake reads all recipes and configurations across all active layers.
3.  **Dependency Mapping**: BitBake creates a "task graph" of what needs to be built first.
4.  **Execution**:
    -   **Fetch**: Downloads source code.
    -   **Unpack**: Extracts sources.
    -   **Patch**: Applies custom patches.
    -   **Configure**: Prepares the build (e.g., `cmake ..`).
    -   **Compile**: Runs `make`.
    -   **Install**: Puts files into a temporary "staging" area.
    -   **Package**: Creates `.ipk`, `.deb`, or `.rpm` files.
    -   **Image**: Assembles the packages into a final filesystem image.

> [!NOTE]
> BitBake is highly intelligent about caching. If a recipe or set of variables hasn't changed, it uses "Shared State" (sstate) to avoid rebuilding, saving hours of build time.
