# 07. SDK & Application Development Flow

You don't want to build the entire Yocto image every time you change one line of C++ code in your camera app. That's what the **SDK** is for.

## 1. What is the SDK?

The SDK (Software Development Kit) is a standalone installer that contains:
- A cross-compiler (e.g., `arm-linux-gnueabihf-gcc`).
- All the header files (`.h`) and libraries (`.so`) that exist on your target image.
- A "sysroot" representing the target environment.

---

## 2. Generating the SDK

Once your image recipe is ready, run:
```bash
bitbake my-ipc-image -c populate_sdk
```
This generates a `.sh` installer in `tmp/deploy/sdk/`.

---

## 3. Using the SDK ("The Traditional Way")

1. Install the SDK: `./oecore-x86_64-armv7at2hf-neon-vfpv4-toolchain-ipc-image.sh`
2. Source the environment: `source /opt/poky/3.x/environment-setup-armv7a...`
3. Now you can run `cmake` or `gcc` directly in your project folder, and it will build for the camera!

---

## 4. The Extensible SDK (eSDK) & `devtool`

The standard SDK is great for simple builds, but `devtool` is the modern way to work.

### 4.1 Modifying an Existing Recipe
```bash
# Setup the environment
devtool modify my-camera-app
```
This clones the source code into a local folder. You can edit it, and then run `bitbake` to only rebuild that specific app.

### 4.2 Creating a New Recipe from Source
```bash
devtool add my-new-app https://github.com/my/project.git
```
`devtool` will analyze the source code and automatically generate a recipe for you!

---

## 5. Integrating with IDEs (VS Code)

To use your Yocto SDK in VS Code:
1. Install the "C/C++" and "CMake Tools" extensions.
2. In your `settings.json`, point the `cmake.cmakePath` to the version in the SDK.
3. Use a "Kit" that sources the environment script before running.

---

## 6. Remote Debugging with `gdbserver`

1. Include `gdbserver` in your image (`IMAGE_INSTALL += "gdbserver"`).
2. On the Camera: `gdbserver :1234 /usr/bin/my-app`
3. On your Host: Run `arm-linux-gnueabihf-gdb` and connect to the camera's IP on port 1234.

> [!TIP]
> Use `devtool ide-sdk` to automatically generate configuration files for VS Code or CLion!
