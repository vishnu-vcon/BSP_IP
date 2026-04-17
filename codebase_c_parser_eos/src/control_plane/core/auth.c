/*
 * core/auth.c — Authentication & Authorisation (Merged v1+v2)
 * =============================================================
 *
 * Token format  : base64url(payload).HMAC-SHA256(payload)   [v2 unchanged]
 * Password store: bcrypt via crypt_r                         [v1 upgrade]
 * User store    : SQLite via auth/db_manager                 [v1 upgrade]
 * 2FA           : email OTP + TOTP (auth/otp_manager + smtp) [v1 new]
 * Logging       : JSON Lines + colour terminal               [v1 new]
 * Permissions   : extended RBAC via auth/role_manager        [v1 extended]
 */

#include "core/auth.h"

/* ── v1 modules ── */
#include "../auth/db_manager.h"
#include "../auth/password_hash.h"
#include "../auth/password_policy.h"
#include "../auth/otp_manager.h"
#include "../auth/smtp_sender.h"
#include "../auth/session_manager.h"
#include "../auth/role_manager.h"
#include "../auth/activity_logger.h"

/* ── GLib / system ── */
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Internal constants
 * ───────────────────────────────────────────────────────────────────────── */

#define QR_OUTPUT_DIR      "/tmp/smartip_qr"

/* ─────────────────────────────────────────────────────────────────────────
 * Opaque handle
 * ───────────────────────────────────────────────────────────────────────── */

struct _TokenAuth {
    char              secret[128];
    int               ttl_sec;
    GHashTable       *revoked;              /* token → time_t* (quick lookup) */

    /* SMTP — loaded from smtp.conf; used to send OTP / TOTP setup emails */
    SmtpServerConfig  smtp;
    gboolean          smtp_loaded;
    char              smtp_recipient[MAX_VALUE_LEN]; /* fallback admin email  */

    /* v2 permission tables (stream / AI commands not in role_manager) */
    const char      **viewer_perms;
    const char      **operator_perms;
};

/* ─────────────────────────────────────────────────────────────────────────
 * v2 stream/AI permission tables
 * role_manager handles device:* and manage:* — these cover the rest.
 * ───────────────────────────────────────────────────────────────────────── */

static const char *VIEWER_PERMS[] = {
    "status", "health", "get_stream_profiles",
    "get_active_streams", "list_models", "get_inference_status", NULL
};

static const char *OPERATOR_PERMS[] = {
    "configure_lens", "stop_lens", "start_stream", "stop_stream",
    "take_snapshot", "load_model", "unload_model",
    "start_recording", "stop_recording", NULL
};

/* ─────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────────────────── */

/* Issue a heap-allocated HMAC token for (username, role). Caller g_free(). */
static char *_issue_token(TokenAuth *auth, const char *username, const char *role)
{
    time_t exp = time(NULL) + auth->ttl_sec;
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"user\":\"%s\",\"role\":\"%s\",\"exp\":%ld}",
             username, role, (long)exp);

    gchar *b64 = g_base64_encode((guchar *)payload, strlen(payload));

    GHmac *hmac = g_hmac_new(G_CHECKSUM_SHA256,
                             (guchar *)auth->secret, strlen(auth->secret));
    g_hmac_update(hmac, (guchar *)b64, strlen(b64));
    const gchar *sig = g_hmac_get_string(hmac);

    char *token = g_strdup_printf("%s.%s", b64, sig);

    g_hmac_unref(hmac);
    g_free(b64);
    return token;
}

/* Copy token from heap string into caller buffer; g_free(heap). */
static AuthLoginResult _deliver_token(char *heap_token,
                                      char *out, int out_len,
                                      const char *username,
                                      const char *client_ip)
{
    if (!heap_token) return AUTH_ERROR;
    g_strlcpy(out, heap_token, out_len);
    g_free(heap_token);
    activity_log_event(username, "LOGIN_SUCCESS", "Token issued", client_ip);
    return AUTH_OK;
}

/* Build a JSON array of policy error strings in caller buffer. */
static void _policy_errors_to_json(char errors[][PASSWORD_ERROR_LEN],
                                   int  count,
                                   char *out, int out_len)
{
    int pos = snprintf(out, out_len, "[");
    for (int i = 0; i < count && pos < out_len - 4; i++) {
        char escaped[PASSWORD_ERROR_LEN * 2];
        int j = 0;
        for (int k = 0; errors[i][k] && j < (int)sizeof(escaped) - 2; k++) {
            if (errors[i][k] == '"' || errors[i][k] == '\\')
                escaped[j++] = '\\';
            escaped[j++] = errors[i][k];
        }
        escaped[j] = '\0';
        pos += snprintf(out + pos, out_len - pos,
                        "%s\"%s\"", i ? "," : "", escaped);
    }
    snprintf(out + pos, out_len - pos, "]");
}

/* Send OTP email if SMTP is configured and recipient is non-empty. */
static gboolean _send_otp_email(TokenAuth  *auth,
                                const char *email,
                                const char *otp)
{
    if (!auth->smtp_loaded) {
        g_warning("[Auth] SMTP not configured — OTP not emailed (code=%s)", otp);
        return FALSE;
    }
    const char *recip[1] = { email };
    int count = 1;
    return email_otp_send(&auth->smtp, recip, count, otp);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ───────────────────────────────────────────────────────────────────────── */

TokenAuth *token_auth_new(const char *secret_key, int ttl_sec,
                          const char *smtp_conf_path)
{
    (void)smtp_conf_path;  /* SMTP now loaded from DB, kept for API compat */

    /* Ensure SQLite schema exists (v1) */
    init_db();

    TokenAuth *auth = g_new0(TokenAuth, 1);

    if (secret_key && *secret_key) {
        g_strlcpy(auth->secret, secret_key, sizeof(auth->secret));
    } else {
        /* Generate random 64-hex secret */
        for (int i = 0; i < 32; i++) {
            guint8 b = (guint8)g_random_int_range(0, 256);
            snprintf(auth->secret + i * 2, 3, "%02x", b);
        }
    }

    auth->ttl_sec       = (ttl_sec > 0) ? ttl_sec : 3600;
    auth->revoked       = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    auth->viewer_perms   = VIEWER_PERMS;
    auth->operator_perms = OPERATOR_PERMS;

    /* Load SMTP config from Database (v1 upgrade: persistent config) */
    if (get_smtp_config(&auth->smtp)) {
        auth->smtp_loaded = TRUE;
        /* Default recipient to the SMTP username if nothing else is set */
        g_strlcpy(auth->smtp_recipient, auth->smtp.username,
                  sizeof(auth->smtp_recipient));
        g_info("[Auth] SMTP credentials loaded from Database");
    } else {
        g_warning("[Auth] SMTP configuration not found in Database — OTP email disabled");
    }

    /* Ensure QR output dir exists */
    g_mkdir_with_parents(QR_OUTPUT_DIR, 0700);

    activity_log_event("system", "SERVER_START", "TokenAuth initialised", NULL);
    return auth;
}

void token_auth_free(TokenAuth *auth)
{
    if (!auth) return;
    g_hash_table_destroy(auth->revoked);
    g_free(auth);
}

void token_auth_update_smtp(TokenAuth *auth, const SmtpServerConfig *cfg)
{
    if (!auth || !cfg) return;
    memcpy(&auth->smtp, cfg, sizeof(SmtpServerConfig));
    auth->smtp_loaded = TRUE;
    g_info("[Auth] SMTP configuration updated dynamically.");
}

/* ─────────────────────────────────────────────────────────────────────────
 * User management
 * ───────────────────────────────────────────────────────────────────────── */

void token_auth_add_user(TokenAuth  *auth,
                         const char *username,
                         const char *password,
                         const char *role,
                         const char *email)
{
    (void)auth; /* SQLite is global in v1 db_manager */

    if (user_exists(username)) {
        g_debug("[Auth] add_user: '%s' already exists, skipping", username);
        return;
    }

    /* Password policy check */
    char errors[PASSWORD_MAX_ERRORS][PASSWORD_ERROR_LEN];
    int  error_count = 0;
    if (!password_policy_check(password, errors, &error_count, PASSWORD_MAX_ERRORS)) {
        g_warning("[Auth] add_user: '%s' — password fails policy: %s",
                  username, errors[0]);
        /* Still insert for pre-seeded accounts; log the warning */
    }

    char hash[BCRYPT_HASH_LEN];
    if (!hash_password(password, hash)) {
        g_error("[Auth] add_user: bcrypt failed for user '%s'", username);
        return;
    }

    insert_user(username, hash, role ? role : "viewer",
                email ? email : "", "");
    g_info("[Auth] User registered: %s (role=%s)", username, role ? role : "viewer");
    activity_log_event(username, "USER_CREATED",
                       role ? role : "viewer", NULL);
}

gboolean token_auth_delete_user(TokenAuth  *auth,
                                const char *username,
                                const char *by_admin,
                                const char *client_ip)
{
    (void)auth;

    if (!user_exists(username)) {
        g_warning("[Auth] delete_user: '%s' not found", username);
        return FALSE;
    }

    delete_user(username);

    char detail[128];
    snprintf(detail, sizeof(detail), "Deleted by admin '%s'", by_admin);
    activity_log_event(username, "USER_REMOVED", detail, client_ip);
    return TRUE;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Single-step login (no 2FA — for programmatic / test use)
 * ───────────────────────────────────────────────────────────────────────── */

char *token_auth_login(TokenAuth *auth, const char *username, const char *password)
{
    UserRecord u;
    if (!get_user(username, &u)) return NULL;
    if (!verify_password(password, u.password_hash)) return NULL;
    return _issue_token(auth, username, u.role);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Two-step 2FA login flow
 * ───────────────────────────────────────────────────────────────────────── */

AuthLoginResult token_auth_begin_login(TokenAuth  *auth,
                                       const char *username,
                                       const char *password,
                                       const char *client_ip,
                                       char       *out_token,
                                       int         out_token_len)
{
    UserRecord u;
    if (!get_user(username, &u)) {
        activity_log_event(username, "LOGIN_FAIL", "User not found", client_ip);
        return AUTH_ERROR;
    }

    if (!verify_password(password, u.password_hash)) {
        activity_log_event(username, "LOGIN_FAIL", "Wrong password", client_ip);
        return AUTH_ERROR;
    }

    /* If user has no email, skip 2FA and issue token immediately */
    if (u.email[0] == '\0') {
        g_warning("[Auth] '%s' has no email — 2FA skipped", username);
        return _deliver_token(_issue_token(auth, username, u.role),
                              out_token, out_token_len, username, client_ip);
    }

    /* Generate OTP and create pending session */
    char otp[OTP_LEN];
    generate_otp(otp);

    PendingSession ps;
    memset(&ps, 0, sizeof(ps));
    g_strlcpy(ps.username, username, USERNAME_MAX);
    g_strlcpy(ps.role,     u.role,   ROLE_MAX);
    g_strlcpy(ps.otp,      otp,      OTP_MAX);
    ps.expires_at = time(NULL) + OTP_TTL_SEC;
    ps.active     = true;
    pending_set(username, &ps);

    /* Send OTP email */
    if (_send_otp_email(auth, u.email, otp)) {
        activity_log_event(username, "OTP_SENT", "Login OTP emailed", client_ip);
    } else {
        /* SMTP failure: still proceed so caller can log / display OTP in dev */
        g_warning("[Auth] '%s' — OTP email failed; code=%s", username, otp);
        activity_log_event(username, "OTP_SEND_FAIL", "SMTP error", client_ip);
    }

    return AUTH_OTP_REQUIRED;
}

AuthLoginResult token_auth_verify_otp(TokenAuth  *auth,
                                      const char *username,
                                      const char *otp,
                                      char       *out_token,
                                      int         out_token_len)
{
    PendingSession ps;
    if (!pending_get(username, &ps)) {
        activity_log_event(username, "OTP_FAIL", "No pending session / expired", NULL);
        return AUTH_ERROR;
    }

    if (g_strcmp0(ps.otp, otp) != 0) {
        activity_log_event(username, "OTP_FAIL", "Wrong OTP", NULL);
        return AUTH_ERROR;
    }

    /* Mark OTP verified */
    ps.otp_verified = true;
    pending_set(username, &ps);

    activity_log_event(username, "OTP_VERIFIED", "Email OTP correct", NULL);

    /* Check if user has TOTP configured */
    UserRecord u;
    if (get_user(username, &u) && u.totp_secret[0] != '\0') {
        return AUTH_TOTP_REQUIRED;
    }

    /* No TOTP — issue token */
    pending_delete(username);
    return _deliver_token(_issue_token(auth, username, ps.role),
                          out_token, out_token_len, username, NULL);
}

AuthLoginResult token_auth_verify_totp(TokenAuth  *auth,
                                       const char *username,
                                       const char *totp_code,
                                       char       *out_token,
                                       int         out_token_len)
{
    PendingSession ps;
    if (!pending_get(username, &ps) || !ps.otp_verified) {
        activity_log_event(username, "TOTP_FAIL",
                           "OTP not yet verified or session expired", NULL);
        return AUTH_ERROR;
    }

    UserRecord u;
    if (!get_user(username, &u) || u.totp_secret[0] == '\0') {
        activity_log_event(username, "TOTP_FAIL", "No TOTP secret on record", NULL);
        return AUTH_ERROR;
    }

    if (!verify_totp(u.totp_secret, totp_code)) {
        activity_log_event(username, "TOTP_FAIL", "Wrong TOTP code", NULL);
        return AUTH_ERROR;
    }

    activity_log_event(username, "TOTP_VERIFIED", "TOTP correct", NULL);
    pending_delete(username);
    return _deliver_token(_issue_token(auth, username, ps.role),
                          out_token, out_token_len, username, NULL);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Token validation & revocation
 * ───────────────────────────────────────────────────────────────────────── */

gboolean token_auth_validate(TokenAuth  *auth,
                             const char *token,
                             char       *out_user, int user_len,
                             char       *out_role, int role_len)
{
    if (!token) return FALSE;
    if (g_hash_table_contains(auth->revoked, token)) return FALSE;

    /* Split at first '.' */
    const char *dot = strchr(token, '.');
    if (!dot) return FALSE;

    int  b64_len = (int)(dot - token);
    char b64[512];
    if (b64_len >= (int)sizeof(b64)) return FALSE;
    memcpy(b64, token, b64_len);
    b64[b64_len] = '\0';

    const char *sig = dot + 1;

    /* Verify HMAC */
    GHmac *hmac = g_hmac_new(G_CHECKSUM_SHA256,
                             (guchar *)auth->secret, strlen(auth->secret));
    g_hmac_update(hmac, (guchar *)b64, b64_len);
    gboolean sig_ok = (g_strcmp0(sig, g_hmac_get_string(hmac)) == 0);
    g_hmac_unref(hmac);
    if (!sig_ok) return FALSE;

    /* Decode payload */
    gsize decoded_len;
    guchar *decoded = g_base64_decode(b64, &decoded_len);
    if (!decoded) return FALSE;

    /* Parse JSON */
    JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser,
                                             (gchar *)decoded, decoded_len, NULL);
    g_free(decoded);
    if (!ok) { g_object_unref(parser); return FALSE; }

    JsonObject *obj  = json_node_get_object(json_parser_get_root(parser));
    const char *user = json_object_get_string_member(obj, "user");
    const char *role = json_object_get_string_member(obj, "role");
    gint64      exp  = json_object_get_int_member(obj, "exp");

    if ((time_t)exp < time(NULL)) {
        g_object_unref(parser);
        return FALSE;
    }

    if (out_user) g_strlcpy(out_user, user ? user : "", user_len);
    if (out_role) g_strlcpy(out_role, role ? role : "", role_len);

    g_object_unref(parser);
    return TRUE;
}

void token_auth_revoke(TokenAuth *auth, const char *token)
{
    time_t *t = g_new(time_t, 1);
    *t = time(NULL);
    g_hash_table_insert(auth->revoked, g_strdup(token), t);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Permission check — v1 role_manager + v2 stream/AI tables
 * ───────────────────────────────────────────────────────────────────────── */

gboolean token_auth_check_perm(TokenAuth  *auth,
                               const char *role,
                               const char *command)
{
    if (!role || !command) return FALSE;

    /* Admin always allowed */
    if (g_strcmp0(role, "admin") == 0) return TRUE;

    /* v1 role_manager covers device:*, manage:*, secure-data, etc. */
    if (check_permission(role, command)) return TRUE;

    /* v2 stream/AI permission tables */
    if (g_strcmp0(role, "operator") == 0) {
        for (int i = 0; auth->operator_perms[i]; i++)
            if (g_strcmp0(command, auth->operator_perms[i]) == 0) return TRUE;
        /* operator inherits viewer */
    }
    for (int i = 0; auth->viewer_perms[i]; i++)
        if (g_strcmp0(command, auth->viewer_perms[i]) == 0) return TRUE;

    return FALSE;
}

/* ─────────────────────────────────────────────────────────────────────────
 * TOTP setup
 * ───────────────────────────────────────────────────────────────────────── */

gboolean token_auth_setup_totp(TokenAuth  *auth,
                               const char *username,
                               const char *client_ip,
                               char       *out_uri,
                               int         out_uri_len)
{
    UserRecord u;
    if (!get_user(username, &u)) return FALSE;

    /* Generate secret */
    char secret[TOTP_SECRET_MAX];
    generate_totp_secret(secret);

    /* Generate QR PNG */
    char qr_path[256];
    char uri[URI_MAX];
    if (!generate_qr_code(username, secret, QR_OUTPUT_DIR, qr_path, uri)) {
        g_warning("[Auth] TOTP QR generation failed for '%s'", username);
        return FALSE;
    }

    /* Persist secret to DB */
    update_totp_secret(username, secret);

    /* Email QR + URI if possible */
    if (auth->smtp_loaded && u.email[0]) {
        const char *recip[1] = { u.email };
        email_otp_send_totp_setup(&auth->smtp, recip, 1, username, qr_path, uri);
    }

    if (out_uri) g_strlcpy(out_uri, uri, out_uri_len);

    activity_log_event(username, "TOTP_SETUP",
                       "TOTP secret generated and saved", client_ip);
    return TRUE;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Password management
 * ───────────────────────────────────────────────────────────────────────── */

gboolean token_auth_change_password(TokenAuth  *auth,
                                    const char *username,
                                    const char *old_pass,
                                    const char *new_pass,
                                    const char *client_ip,
                                    char       *error_json_out,
                                    int         error_json_len)
{
    (void)auth;

    UserRecord u;
    if (!get_user(username, &u)) return FALSE;

    if (!verify_password(old_pass, u.password_hash)) {
        if (error_json_out)
            g_strlcpy(error_json_out, "[\"Current password is incorrect\"]",
                      error_json_len);
        activity_log_event(username, "PW_CHANGE_FAIL",
                           "Wrong current password", client_ip);
        return FALSE;
    }

    char errors[PASSWORD_MAX_ERRORS][PASSWORD_ERROR_LEN];
    int  count = 0;
    if (!password_policy_check(new_pass, errors, &count, PASSWORD_MAX_ERRORS)) {
        if (error_json_out)
            _policy_errors_to_json(errors, count, error_json_out, error_json_len);
        activity_log_event(username, "PW_CHANGE_FAIL",
                           "Policy violation", client_ip);
        return FALSE;
    }

    char new_hash[BCRYPT_HASH_LEN];
    if (!hash_password(new_pass, new_hash)) return FALSE;

    update_password_hash(username, new_hash);
    activity_log_event(username, "PW_CHANGED", "Password updated", client_ip);
    return TRUE;
}

gboolean token_auth_forgot_password(TokenAuth  *auth,
                                    const char *username,
                                    const char *client_ip)
{
    UserRecord u;
    if (!get_user(username, &u) || u.email[0] == '\0') {
        /* Don't reveal whether username exists */
        return TRUE;
    }

    char otp[OTP_LEN];
    generate_otp(otp);

    PendingSession ps;
    memset(&ps, 0, sizeof(ps));
    g_strlcpy(ps.username, username, USERNAME_MAX);
    g_strlcpy(ps.role,     u.role,   ROLE_MAX);
    g_strlcpy(ps.otp,      otp,      OTP_MAX);
    g_strlcpy(ps.action,   "reset_password", ACTION_MAX);
    ps.expires_at = time(NULL) + OTP_TTL_SEC;
    ps.active     = true;
    pending_set(username, &ps);

    _send_otp_email(auth, u.email, otp);
    activity_log_event(username, "FORGOT_PW", "Reset OTP sent", client_ip);
    return TRUE;
}

AuthLoginResult token_auth_reset_password(TokenAuth  *auth,
                                          const char *username,
                                          const char *otp,
                                          const char *new_pass,
                                          char       *error_json_out,
                                          int         error_json_len)
{
    (void)auth;

    PendingSession ps;
    if (!pending_get(username, &ps) ||
        g_strcmp0(ps.action, "reset_password") != 0) {
        activity_log_event(username, "RESET_PW_FAIL",
                           "No pending reset / expired", NULL);
        return AUTH_ERROR;
    }

    if (g_strcmp0(ps.otp, otp) != 0) {
        activity_log_event(username, "RESET_PW_FAIL", "Wrong OTP", NULL);
        return AUTH_ERROR;
    }

    char errors[PASSWORD_MAX_ERRORS][PASSWORD_ERROR_LEN];
    int  count = 0;
    if (!password_policy_check(new_pass, errors, &count, PASSWORD_MAX_ERRORS)) {
        if (error_json_out)
            _policy_errors_to_json(errors, count, error_json_out, error_json_len);
        activity_log_event(username, "RESET_PW_FAIL",
                           "New password fails policy", NULL);
        return AUTH_ERROR;
    }

    char new_hash[BCRYPT_HASH_LEN];
    if (!hash_password(new_pass, new_hash)) return AUTH_ERROR;

    update_password_hash(username, new_hash);
    pending_delete(username);
    activity_log_event(username, "PW_RESET", "Password reset via OTP", NULL);
    return AUTH_OK;
}
