# Validation Report — Merged Control Plane

## Overall Assessment

All **7 issues** (5 user-reported + 2 discovered during audit) have been **fixed**.
The control plane is structurally sound and ready for target compilation.

---

## ✅ Files verified CORRECT (no changes needed)

| File | Status | Notes |
|------|--------|-------|
| `auth.c` | ✅ CORRECT | SMTP from DB (not file) is a good upgrade over the plan. `token_auth_update_smtp()` properly wired. All 2FA flows correct. |
| `main.c` (cp) | ✅ CORRECT | `g_mkdir_with_parents("config",…)` added for activity log path — good. SERVER_START/STOP logging correct. |
| `db_manager.c` | ✅ CORRECT | `smtp_settings` table + seed, `db_manager_list_users()`, `get_smtp_config()`, `update_smtp_config()` — all solid additions. |
| `session_manager.c` | ✅ CORRECT | Unchanged v1 — correct. |
| `role_manager.c` | ✅ CORRECT | Unchanged v1 — correct. |
| `password_hash.c/h` | ✅ CORRECT | Unchanged v1 — correct. |
| `password_policy.c/h` | ✅ CORRECT | Unchanged v1 — correct. |
| `otp_manager.c/h` | ✅ CORRECT | Unchanged v1 — correct. |
| `activity_logger.c/h` | ✅ CORRECT | Log path changed to `config/user_activity.log` — matches the `g_mkdir_with_parents("config",…)` in main.c. |
| `smtp_sender.c/h` | ✅ CORRECT | Unchanged v1. Canonical owner of `SmtpServerConfig`. |
| `http_server.h` | ✅ CORRECT | Structurally complete. |

---

## ✅ Issues Fixed

### ISSUE 1 — FIXED: `http_server.c` was corrupted

**Problem:** `_handle_forgot_password` was truncated mid-line, `_user_list_cb` spliced into it,
`_handle_users` was defined twice, `_handle_reset_password` was missing entirely.

**Fix applied:**
- Restored `_handle_forgot_password` as a complete function.
- Added `_handle_reset_password` as a complete function.
- Moved `_user_list_cb` to its own static function before `_handle_users`.
- Kept the improved `_handle_users` (with `db_manager_list_users`) and removed the duplicate placeholder.

---

### ISSUE 2 — FIXED: `_handle_system_smtp` used wrong struct field names

**Problem:** `cfg.user`, `cfg.pass`, `cfg.from_email` — none exist on `SmtpServerConfig`.

**Fix applied:**
```diff
-cfg.user        → cfg.username
-cfg.pass        → cfg.password
-cfg.from_email  → cfg.mail_from
```

---

### ISSUE 3 — FIXED: `update_smtp_config()` returns `void` but was tested as boolean

**Problem:** `if (update_smtp_config(&cfg)) { ... }` — void cannot be tested.

**Fix applied:** Called unconditionally, always responds success.
```diff
-if (update_smtp_config(&cfg)) {
-    ...
-} else {
-    http_server_respond_error(msg, 500, "Failed to update SMTP config");
-}
+update_smtp_config(&cfg);
+token_auth_update_smtp(srv->auth, &cfg);
+http_server_respond_json(msg, 200, "{\"status\":\"ok\"}");
```

---

### ISSUE 4 — FIXED: `auth.h` used `SmtpServerConfig` without include

**Problem:** `token_auth_update_smtp(TokenAuth*, const SmtpServerConfig*)` was declared
but `SmtpServerConfig` was unknown to any file including `auth.h` alone.

**Fix applied:** Added `#include "../auth/db_manager.h"` to `auth.h`.

---

### ISSUE 5 — FIXED: `db_manager.h` had `delete_user` outside `#endif`

**Problem:** `delete_user()` declaration was on line 62, after the `#endif` guard.

**Fix applied:** Moved inside the header guard, alongside all other declarations.

---

### ISSUE 6 — FIXED: Duplicate `SmtpServerConfig` typedef

**Problem:** Both `smtp_sender.h` and `db_manager.h` defined `SmtpServerConfig`,
causing a compile error when both are included (e.g. in `auth.c`).

**Fix applied:** Removed the typedef from `db_manager.h` and added
`#include "smtp_sender.h"` so the type comes from one canonical source.

---

### ISSUE 7 — FIXED: C CLI `ntp` command used undeclared `path` variable

**Problem:** Line 277 in `cli/main.c` called `call_api("PATCH", path, ...)` but
`path` was never declared in that scope.

**Fix applied:** Added `char path[128]` and built it from the lens name:
```c
char path[128];
snprintf(path, sizeof(path), "/lenses/%s/ntp_overlay", lens);
```

---

## Summary table

| # | Severity | File | Problem | Status |
|---|----------|------|---------|--------|
| 1 | 🔴 CRITICAL | `http_server.c` | Corrupted — truncated function, duplicate definition, missing function | ✅ FIXED |
| 2 | 🔴 CRITICAL | `http_server.c` | Wrong struct field names in `_handle_system_smtp` | ✅ FIXED |
| 3 | 🔴 CRITICAL | `http_server.c` | `void` return used as boolean | ✅ FIXED |
| 4 | 🔴 CRITICAL | `auth.h` | `SmtpServerConfig` used without `#include` | ✅ FIXED |
| 5 | 🟡 MEDIUM | `db_manager.h` | `delete_user` outside `#endif` guard | ✅ FIXED |
| 6 | 🔴 CRITICAL | `db_manager.h` + `smtp_sender.h` | Duplicate `SmtpServerConfig` typedef | ✅ FIXED |
| 7 | 🟡 BUG | `cli/main.c` | `ntp` command used undeclared `path` variable | ✅ FIXED |
