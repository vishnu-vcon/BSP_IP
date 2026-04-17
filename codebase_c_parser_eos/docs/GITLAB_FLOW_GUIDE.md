# GitLab Flow & CI Verification Guide — SmartIP Edge

This project uses a "GitLab Flow" inspired development process optimized for embedded hardware development. This guide explains how to develop, test, and merge your changes securely.

## 1. Parallel Development Workflow

To ensure stability while developing in parallel:

1.  **Clone/Branch**: Always create a feature branch from `main`.
    ```bash
    git checkout -b feature/your-feature-name
    ```
2.  **Hardware Testing**: Since this is a hardware project, you **must** test your changes on the i.MX8MP board before pushing.
    - Use `build_on_board.sh` to compile.
    - Run your logic and verify GStreamer pipelines manually.
3.  **Push**: Once verified on hardware, push your branch to GitLab.
    ```bash
    git push origin feature/your-feature-name
    ```
4.  **Merge Request**: Create a Merge Request (MR) in GitLab. This triggers the **Automated Verification Pipeline**.

## 2. Automated Verification (CI)

Our GitLab CI is configured to run specialized "Verification Scripts" that confirm internal function call flows and parameter expectations. This acts as a "safety net" to catch regressions even if you can't run a full hardware simulation in CI.

### What is verified?
- **Authentication Flow**: Verifies that the login process correctly calls HMAC cryptographic functions with the right secrets and payloads.
- **Pipeline Construction**: Verifies that GStreamer elements (queues, encoders, parsers) are created in the correct sequence with the expected names.
- **Parameter Expectations**: Checks that functions receive the correct input parameters (e.g., bitrates, resolutions).

### Running Tests Locally
You can run these verification scripts locally to ensure your MR will pass:

```bash
# Run the verification suite
chmod +x scripts/build_tests.sh && ./scripts/build_tests.sh
```

## 3. Merge Confirmation

1.  **CI Success**: The pipeline must pass all verification scripts.
2.  **Peer Review**: Another developer must review the code.
3.  **Hardware Confirmation**: High-risk changes may require a "Hardware TSL (Tested on Live)" label before merge.
4.  **Merge**: Once confirmed, the UI/Lead will merge the branch into `main`.

---
**Note**: The verification scripts use `cmocka` and the linker's `--wrap` feature to mock hardware/system dependencies. Do not modify the core project logic just to satisfy a test; instead, update the test mocks if the API changes.
