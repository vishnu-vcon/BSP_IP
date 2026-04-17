# Control Plane — v1 + v2 Merge Notes

## What changed and why

### `core/auth.h` / `core/auth.c`
| Area | v2 (before) | v1+v2 (merged) |
|------|-------------|----------------|
| Password hashing | SHA-256 via GLib GChecksum | **bcrypt** via `crypt_r` (auth/password_hash) |
| User store | In-memory `GHashTable` | **SQLite** via `auth/db_manager` — survives restarts |
| Password policy | None | **Complexity rules** via `auth/password_policy` (≥8 chars, upper, lower, digit, special) |
| Login flow | Single-step → token | **2-step 2FA**: phase-1 sends email OTP; phase-2a verifies OTP; phase-2b verifies TOTP |
| TOTP | None | **RFC 6238 TOTP** — secret generation, QR PNG via libqrencode, email delivery |
| Activity logging | `g_info/g_warning` only | **JSON Lines** to `/data/device_logs/user_activity.log` + colour-coded terminal mirror |
| RBAC | Inline viewer/operator tables | **Extended**: delegates to `auth/role_manager` for `device:*` / `manage:*`; v2 tables cover stream/AI |
| Token format | HMAC-SHA256 JWT-style | **Unchanged** — kept for GLib/libsoup compatibility |

`token_auth_new()` now takes an optional `smtp_conf_path` argument (pass `NULL` for the default `/etc/smartip/smtp.conf`). SMTP failures are non-fatal — the server starts without email.

`token_auth_login()` is kept unchanged for backward-compatible single-step use (no 2FA). The HTTP handler now calls `token_auth_begin_login()` which starts the 2FA flow.

---

### `core/http_server.h` / `core/http_server.c`

New endpoints registered in `http_server_new()`:

```
POST /api/v1/auth/verify-otp       {username, otp}           → {token} or {status:"totp_required"}
POST /api/v1/auth/verify-totp      {username, totp_code}     → {token}
POST /api/v1/auth/setup-totp       (Bearer) (no body)        → {uri}
POST /api/v1/auth/logout           (Bearer) (no body)        → {status:"logged_out"}
POST /api/v1/auth/change-password  (Bearer) {old_password, new_password}
POST /api/v1/auth/forgot-password  {username}                → always 200 (no enumeration)
POST /api/v1/auth/reset-password   {username, otp, new_password}

GET    /api/v1/users               (admin)  → {users:[...]}
POST   /api/v1/users               (admin)  {username,password,role,email}
DELETE /api/v1/users/<username>    (admin)

GET  /api/v1/device/status         (viewer)
POST /api/v1/device/reboot         (admin)
POST /api/v1/device/network-reset  (operator)
POST /api/v1/device/config-reset   (admin)
POST /api/v1/device/factory-reset  (admin)  {confirm:true}
```

`http_server_authorize_ex()` added — same as `http_server_authorize()` but also returns the authenticated `user` and `role` into caller buffers. Used by device/user handlers that need identity for logging.

`_handle_login` updated to call `token_auth_begin_login()`.  Returns HTTP 202 + `{status:"otp_required"}` instead of a token when 2FA is required.

---

### `main.c`

- `token_auth_new()` call updated to pass `smtp_conf` (new `--smtp-conf` CLI flag).
- `token_auth_add_user()` updated to 5-arg form (adds `email` param; pass `""` to skip 2FA for that user).
- `activity_log_event()` called on `SERVER_START` and `SERVER_STOP`.
- The `soup_server_add_handler` override pattern for lenses/system/recordings is preserved exactly.
- `--smtp-conf PATH` added as optional CLI argument.

---

## Directory layout expected by compiler

```
control_plane/
├── main.c                     ← REPLACED
├── web_assets.h               ← unchanged
├── device_controls.h          ← v1 (unchanged)
├── device_controls.c          ← v1 (unchanged)
├── core/
│   ├── auth.h                 ← REPLACED
│   ├── auth.c                 ← REPLACED
│   ├── http_server.h          ← REPLACED
│   └── http_server.c          ← REPLACED
├── auth/                      ← v1 modules (all unchanged)
│   ├── activity_logger.h/c
│   ├── db_manager.h/c
│   ├── otp_manager.h/c
│   ├── password_hash.h/c
│   ├── password_policy.h/c
│   ├── role_manager.h/c
│   ├── session_manager.h/c
│   └── smtp_sender.h          (+ smtp_sender.c — compile separately)
└── common/
    ├── config.h
    └── event_broker.h
```

## Build additions (link flags)

Add to your existing link flags:

```
-lsqlite3   # db_manager
-lcrypt      # password_hash (bcrypt)
-lcrypto     # otp_manager (OpenSSL HMAC-SHA1 for TOTP)
-lqrencode   # otp_manager (QR PNG)
-lpng        # otp_manager (write PNG)
-lcurl       # smtp_sender (SMTP via libcurl)
```

Packages on Debian/Ubuntu:
```bash
sudo apt-get install libsqlite3-dev libxcrypt-dev libssl-dev \
                     libqrencode-dev libpng-dev libcurl4-openssl-dev
```

## 2FA login flow (API sequence)

```
POST /api/v1/auth/login
  {"username":"alice","password":"..."}
  → 202 {"status":"otp_required","username":"alice"}

POST /api/v1/auth/verify-otp
  {"username":"alice","otp":"123456"}
  → 202 {"status":"totp_required"}       # if TOTP is configured
  → 200 {"token":"<hmac-token>"}         # if no TOTP

POST /api/v1/auth/verify-totp
  {"username":"alice","totp_code":"654321"}
  → 200 {"token":"<hmac-token>"}
```

Users with no email address bypass 2FA and receive a token directly from `/login` (HTTP 200).
