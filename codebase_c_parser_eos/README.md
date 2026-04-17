# SmartIP Edge Camera Engine (C Port)

This repository contains the C-based core engine and control plane for the SmartIP Edge camera system, ported from the original Python implementation for optimized performance and minimal memory footprint on the i.MX8MP (NXP) hardware.

## 📁 Directory Structure

- **`/src`**: Core source code (Engine, Control Plane, Alert Manager, CLI).
- **`/scripts`**: Build and deployment scripts.
- **`/docs`**: Technical documentation and guides.
- **`/tests`**: Automated verification suite (Mocked logic testing).
- **`/config`**: Default system configurations.

## 🚀 Getting Started

### 1. Build on Hardware (Target)
To compile the project on the i.MX8MP board:
```bash
chmod +x scripts/build_on_board.sh
./scripts/build_on_board.sh
```

### 2. Run Local Verification
Ensure logic and contract consistency without hardware:
```bash
chmod +x scripts/build_tests.sh
./scripts/build_tests.sh
```

## 📜 Development Flow
This project follows a strict **GitLab Flow**. Please refer to the [GitLab Flow Guide](docs/GITLAB_FLOW_GUIDE.md) before pushing any changes or requesting a merge.

## 🛠️ Verification
All Merge Requests trigger an automated CI pipeline that runs `cmocka`-based verification scripts. These scripts mock hardware and external dependencies to verify internal function call flows and parameter expectations.

---
**Core Team**: Vishnuvardhan Siddavatam
