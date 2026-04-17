/*
 * core/auth.h — Authentication & Authorisation (Merged v1+v2)
 * =============================================================
 *
 * v2 base: HMAC-SHA256 token format, GLib/libsoup integration.
 * v1 additions:
 *   - bcrypt password hashing  (via auth/password_hash.h)
 *   - SQLite user persistence  (via auth/db_manager.h)
 *   - Password-complexity policy (via auth/password_policy.h)
 *   - 2-factor auth: email OTP + TOTP (via auth/otp_manager.h + smtp_sender.h)
 *   - Activity logging         (via auth/activity_logger.h)
 *   - Extended RBAC            (via auth/role_manager.h)
 */

#ifndef SMARTIP_AUTH_H
#define SMARTIP_AUTH_H

#include <glib.h>
#include <stdbool.h>

/* ── Opaque handle ─────────────────────────────────────────────────────── */

typedef struct _TokenAuth TokenAuth;

/* ── 2-FA login result ─────────────────────────────────────────────────── */

typedef enum {
    AUTH_OK,             /* token issued — out_token filled                */
    AUTH_OTP_REQUIRED,   /* email OTP sent — awaiting /verify-otp          */
    AUTH_TOTP_REQUIRED,  /* OTP verified — awaiting /verify-totp           */
    AUTH_ERROR           /* bad credentials / expired / policy fail        */
} AuthLoginResult;

/* out_token buffer size — must be at least this large for token functions */
#define AUTH_TOKEN_BUF 512

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/*
 * Create TokenAuth.  Calls init_db() to ensure SQLite schema exists.
 * Attempts to load SMTP config from smtp_conf_path (may be NULL → default
 * path /etc/smartip/smtp.conf).  SMTP errors are non-fatal.
 */
TokenAuth  *token_auth_new (const char *secret_key, int ttl_sec,
                            const char *smtp_conf_path);

void        token_auth_free(TokenAuth *auth);

/* ── User management ───────────────────────────────────────────────────── */

/*
 * Add a user.  Password is bcrypt-hashed and stored in SQLite.
 * Silently skips if username already exists.
 * email may be NULL/empty (user will bypass OTP step if so).
 */
void token_auth_add_user(TokenAuth *auth,
                         const char *username,
                         const char *password,
                         const char *role,
                         const char *email);

/*
 * Delete a user from SQLite.
 * by_admin — username of the admin performing the action (for logging).
 * client_ip may be NULL.
 */
gboolean token_auth_delete_user(TokenAuth *auth,
                                const char *username,
                                const char *by_admin,
                                const char *client_ip);

/* ── Authentication — single-step (no 2FA, kept for internal use) ──────── */

/*
 * Validate credentials and return a heap-allocated HMAC token (caller
 * must g_free), or NULL on failure.  Bypasses 2FA — use for programmatic
 * or test flows only.
 */
char *token_auth_login(TokenAuth *auth,
                       const char *username,
                       const char *password);

/* ── Authentication — two-step 2FA flow ───────────────────────────────── */

/*
 * Phase 1: validate credentials, generate and email OTP, create pending
 * session.  If the user has no email address, falls back to AUTH_OK and
 * writes the token into out_token (out_token_len must be AUTH_TOKEN_BUF).
 */
AuthLoginResult token_auth_begin_login(TokenAuth      *auth,
                                       const char     *username,
                                       const char     *password,
                                       const char     *client_ip,
                                       char           *out_token,
                                       int             out_token_len);

/*
 * Phase 2a: verify the emailed OTP.
 * - If the user has a TOTP secret stored → AUTH_TOTP_REQUIRED (mark
 *   otp_verified in pending session; caller shows TOTP input).
 * - Otherwise → AUTH_OK, token written to out_token.
 */
AuthLoginResult token_auth_verify_otp(TokenAuth  *auth,
                                      const char *username,
                                      const char *otp,
                                      char       *out_token,
                                      int         out_token_len);

/*
 * Phase 2b: verify TOTP code (authenticator app).
 * Returns AUTH_OK with token, or AUTH_ERROR.
 */
AuthLoginResult token_auth_verify_totp(TokenAuth  *auth,
                                       const char *username,
                                       const char *totp_code,
                                       char       *out_token,
                                       int         out_token_len);

/* ── Token validation & revocation ─────────────────────────────────────── */

gboolean token_auth_validate(TokenAuth  *auth,
                             const char *token,
                             char       *out_user, int user_len,
                             char       *out_role, int role_len);

void     token_auth_revoke(TokenAuth *auth, const char *token);

/* ── Permission check ──────────────────────────────────────────────────── */

/*
 * Returns TRUE if role may perform command.
 * Delegates to v1 role_manager for device/manage permissions;
 * falls back to v2 inline tables for stream/AI commands.
 */
gboolean token_auth_check_perm(TokenAuth  *auth,
                               const char *role,
                               const char *command);

/* ── TOTP setup ────────────────────────────────────────────────────────── */

/*
 * Generate a TOTP secret + QR code PNG for username, save secret to DB,
 * and (if SMTP is configured) email the QR and URI to the user.
 * out_uri must be at least 512 bytes.
 * Returns TRUE on success.
 */
gboolean token_auth_setup_totp(TokenAuth  *auth,
                               const char *username,
                               const char *client_ip,
                               char       *out_uri,
                               int         out_uri_len);

/* ── Password management ───────────────────────────────────────────────── */

/*
 * Change password after verifying old_pass.
 * Runs new_pass through policy checks.
 * error_json_out (caller-allocated, ≥1024 bytes) receives a JSON array of
 * policy error strings on failure, e.g. ["Must be ≥ 8 chars",...].
 * Returns TRUE on success.
 */
gboolean token_auth_change_password(TokenAuth  *auth,
                                    const char *username,
                                    const char *old_pass,
                                    const char *new_pass,
                                    const char *client_ip,
                                    char       *error_json_out,
                                    int         error_json_len);

/*
 * Forgot-password: send a reset OTP to the user's registered email.
 * Returns TRUE if the OTP was sent (user exists and has an email).
 */
gboolean token_auth_forgot_password(TokenAuth  *auth,
                                    const char *username,
                                    const char *client_ip);

/*
 * Reset password after verifying the reset OTP.
 * Returns AUTH_OK on success, AUTH_ERROR on bad OTP / policy fail.
 * error_json_out same contract as token_auth_change_password.
 */
AuthLoginResult token_auth_reset_password(TokenAuth  *auth,
                                          const char *username,
                                          const char *otp,
                                          const char *new_pass,
                                          char       *error_json_out,
                                          int         error_json_len);

#endif /* SMARTIP_AUTH_H */
