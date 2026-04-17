# /scripts — Build & Utility Scripts

This directory contains shell scripts for building, testing, and managing the SmartIP Edge Camera Engine.

## 🛠️ Scripts Overview

- **`build_on_board.sh`**: Compiles all core binaries (`smartip_engine`, `smartip_control`, etc.) using `gcc`. Use this on the i.MX8MP target.
- **`build_tests.sh`**: Compiles and runs the `cmocka` verification suite. This uses a mocked environment to verify logic without hardware.

## 🚀 Usage

All scripts are designed to be run from the **project root**:

```bash
# Example
./scripts/build_tests.sh
```
