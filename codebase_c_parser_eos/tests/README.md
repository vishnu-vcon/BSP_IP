# /tests — Automated Verification Suite

This directory contains verification scripts and mock infrastructure to ensure the C port's logic matches the original Python expectations.

## 🧪 Testing Philosophy
Since this is a hardware-intensive project, we use **Mocked Verification** to catch logic errors in CI before deploying to the board. We use the GNU linker's `--wrap` feature to intercept system/library calls.

## 📁 Structure
- **`verify_auth_flow.c`**: Tests HMAC security logic.
- **`verify_pipeline_logic.c`**: Tests GStreamer branch construction.
- **`mocks/`**: Dummy headers and stubs for missing hardware libraries (ZMQ, GStreamer, Json-GLib).

## 🚀 Running Tests
```bash
./scripts/build_tests.sh
```
