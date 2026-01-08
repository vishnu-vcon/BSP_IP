# 03. Writing Recipes: A Deep Dive

A Recipe (`.bb` file) is a set of instructions. It usually consists of metadata at the top and tasks at the bottom.

## 1. Minimal Recipe Structure

```python
SUMMARY = "A short description"
DESCRIPTION = "A longer, detailed description"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Where to get the source
SRC_URI = "git://github.com/example/mypackage.git;protocol=https;branch=main"
SRCREV = "abc123def..." # Specific commit hash

# Where the build happens
S = "${WORKDIR}/git"

# List dependencies
DEPENDS = "libpng zlib"
RDEPENDS:${PN} = "bash" # Runtime dependency
```

---

## 2. Standard Tasks (The Execution Flow)

BitBake runs these tasks in order. You can override them if needed.

| Task | Purpose |
| :--- | :--- |
| `do_fetch` | Downloads the source from `SRC_URI`. |
| `do_unpack` | Extracts source to `${WORKDIR}`. |
| `do_patch` | Applies any `.patch` files listed in `SRC_URI`. |
| `do_configure` | Usually runs `./configure`, `cmake`, etc. |
| `do_compile` | Runs `make` or the build command. |
| `do_install` | Copies files from build dir to `${D}` (Destination). |
| `do_package` | Splits files into packages (doc, dev, dbg, main). |

---

## 3. Using Classes (`inherit`)

Classes simplify recipes by providing default task implementations.

-   **`inherit autotools`**: Automatically handles `./configure && make && make install`.
-   **`inherit cmake`**: Handles `cmake && make && make install`.
-   **`inherit python_setuptools_build_meta`**: For Python projects.
-   **`inherit systemd`**: For projects that install systemd service files.

---

## 4. The `do_install` Task (Critical for Beginners)

When you write your own app, you MUST tell Yocto where to put the files on the final device.

```python
do_install() {
    # 1. Create the directory in the destination tree (${D})
    install -d ${D}${bindir}
    install -d ${D}${sysconfdir}

    # 2. Copy the binary
    install -m 0755 ${B}/my_binary ${D}${bindir}/

    # 3. Copy a config file
    install -m 0644 ${S}/my_config.conf ${D}${sysconfdir}/
}
```

- `${bindir}`: Usually `/usr/bin`
- `${sysconfdir}`: Usually `/etc`
- `${libdir}`: Usually `/usr/lib`

---

## 5. Recipe Naming Convention

File should be named: `<package-name>_<version>.bb`
Example: `my-camera-app_1.2.bb`

- `PN` (Package Name) = `my-camera-app`
- `PV` (Package Version) = `1.2`

> [!TIP]
> Use `_${PV}` in your `SRC_URI` if you want to reuse the version number in the download link!
