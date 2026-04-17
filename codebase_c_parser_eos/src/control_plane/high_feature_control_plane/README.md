# SmartCamera Auth Server — C Port

Full conversion of the Python/Flask `auth_project_v3` to pure C.

## File map

| C file | Converted from |
|--------|---------------|
| `server.c` | `server.py` (Flask → libmicrohttpd) |
| `device_controls.c` | `device_controls.py` |
| `auth/password_policy.c` | `auth/password_policy.py` |
| `auth/password_hash.c` | `auth/password_hash.py` (bcrypt via libxcrypt) |
| `auth/role_manager.c` | `auth/role_manager.py` |
| `auth/session_manager.c` | `auth/session_manager.py` |
| `auth/db_manager.c` | `auth/db_manager.py` (sqlite3 C API) |
| `auth/otp_manager.c` | `auth/otp_manager.py` (TOTP via OpenSSL HMAC-SHA1) |
| `auth/activity_logger.c` | `auth/activity_logger.py` |

## Dependencies

```
sudo apt-get install \
    libmicrohttpd-dev \   # HTTP server (replaces Flask)
    libsqlite3-dev \      # SQLite (same DB as Python version)
    libcurl4-openssl-dev \ # SMTP email (replaces smtplib)
    libqrencode-dev \     # QR code PNG (replaces qrcode Python lib)
    libpng-dev \          # PNG writing (used by otp_manager.c)
    libssl-dev \          # HMAC-SHA1 for TOTP (replaces pyotp)
    libxcrypt-dev         # bcrypt (replaces Python bcrypt)
```

Also download **cJSON** (single-file JSON library):
```
wget https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
wget https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
```

## Build

```bash
make
```

## Run

```bash
./smartcamera_server
# Listens on 0.0.0.0:8000
```

## API Routes (identical to Python version)

| Method | Route | Description |
|--------|-------|-------------|
| GET  | `/` | Serve UI (auth_test.html) |
| POST | `/login` | Step 1: password check |
| POST | `/send-email-otp` | Send 6-digit email OTP |
| POST | `/verify-otp` | Verify email OTP → issue token |
| POST | `/verify-totp` | Verify Authenticator code → issue token |
| POST | `/resend-totp-setup` | Re-send QR code setup email |
| POST | `/forgot-password` | Start forgot-password flow |
| POST | `/forgot-send-email` | Send reset OTP |
| POST | `/forgot-verify-otp` | Verify reset OTP |
| POST | `/forgot-verify-totp` | Verify reset TOTP |
| POST | `/reset-password` | Set new password |
| GET  | `/secure-data` | Protected data (requires token) |
| POST | `/start-stream` | Start GStreamer camera stream |
| POST | `/stop-stream` | Stop stream |
| POST | `/device/reboot` | Reboot device |
| GET  | `/device/status` | Uptime, storage, RAM, network |
| POST | `/device/network-reset` | Restart networking |
| POST | `/device/config-reset` | Restore config defaults |
| POST | `/device/factory-reset` | Factory reset (requires `confirm:true`) |
| POST | `/add-user` | Admin: create new user |

## Key implementation notes

- **HTTP server**: `libmicrohttpd` (thread-per-connection mode) replaces Flask + CORS.
- **bcrypt**: `crypt_r()` from glibc's `libxcrypt` with `$2b$12$` salt prefix.
- **TOTP**: RFC 6238 implemented directly using `OpenSSL HMAC-SHA1`; no pyotp dependency.
- **QR code**: `libqrencode` + `libpng` write the PNG files directly.
- **Email**: `libcurl` SMTP with STARTTLS replaces Python `smtplib`.
- **JSON**: `cJSON` (single-header/source) replaces Flask's `jsonify`.
- **Sessions**: in-memory arrays (same semantics as Python dicts); restart clears sessions.
- **DB**: same `users.db` SQLite file — binary compatible with the Python version.
