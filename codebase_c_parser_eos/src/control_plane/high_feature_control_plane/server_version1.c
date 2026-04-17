/*
 * server.c — SmartCamera HTTP Server (behind NGINX)
 *
 * Implements all auth, camera, and device-control routes.
 * Listens on plain HTTP :8080 — NGINX handles TLS on :443.
 * Uses cJSON for JSON parsing/building.
 *
 * Build:
 *   sudo apt install libmicrohttpd-dev \
 *       libsqlite3-dev libcurl4-openssl-dev libqrencode-dev \
 *       libpng-dev libssl-dev libxcrypt-dev
 *
 *   make
 *
 * Run:
 *   ./smartcamera_server
 *   Listens on http://127.0.0.1:8080  (NGINX proxies to here)
 *
 * NGINX handles:
 *   - TLS termination on :443 using OpenSSL
 *   - Certificate: /etc/nginx/ssl/smartcamera.crt
 *   - Key:         /etc/nginx/ssl/smartcamera.key
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

#include <microhttpd.h>
#include "cJSON.h"

#include "auth/password_policy.h"
#include "auth/password_hash.h"
#include "auth/role_manager.h"
#include "auth/session_manager.h"
#include "auth/db_manager.h"
#include "auth/otp_manager.h"
#include "auth/smtp_sender.h"
#include "auth/activity_logger.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "device_controls.h"

/* report_log_writer.h removed — log_alarm/log_init not used; activity_logger handles all logging */

/* ── Configuration ──────────────────────────────────────────────── */
#define SERVER_PORT      8080             /* plain HTTP — NGINX proxies here */
#define QR_DIR           "./qrcodes"
#define HTML_FILE        "./auth_test.html"
#define RESP_BUF_SIZE    16384
#define JSON_BUF_SIZE     4096
#define LOG_DIR          "/data/device_logs"

/* SMTP password is loaded from environment at startup (see main).
 * Set it with:  export SMTP_PASSWORD="your_app_password"
 */
static SmtpServerConfig g_email_cfg = {
    .host       = "smtp.gmail.com",
    .port       = 587,
    .use_ssl    = false,
    .username   = "swethavc47@gmail.com",
    .password   = "oxtqlxwdqcbctkwr",     /* filled in main() from SMTP_PASSWORD env var */
    .mail_from  = "swethavc47@gmail.com"
};

static const char *USER_ROLES[] = { "admin", "operator", "viewer", NULL };

static pid_t           stream_pid   = -1;  /* camera stream child process */
static pthread_mutex_t stream_mutex = PTHREAD_MUTEX_INITIALIZER; /* protects stream_pid */

/* ── Timestamp Helper ────────────────────────────────────────────── */
/*
 * get_timestamp() — fills buf with current local time
 * Format: "2026-04-01 12:30:45"
 */
static void get_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", t);
}


/* ── Helpers ────────────────────────────────────────────────────── */

/* Send a JSON response */
static enum MHD_Result send_json(struct MHD_Connection *conn,
                                  unsigned int status,
                                  const char *json_str)
{
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(strlen(json_str),
                                        (void *)json_str,
                                        MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    enum MHD_Result r = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return r;
}

static enum MHD_Result json_error(struct MHD_Connection *conn,
                                   unsigned int status,
                                   const char *msg)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return send_json(conn, status, buf);
}

/* Get client IP */
static const char *client_ip(struct MHD_Connection *conn)
{
    const char *fwd = MHD_lookup_connection_value(
        conn, MHD_HEADER_KIND, "X-Forwarded-For");
    if (fwd) return fwd;
    const union MHD_ConnectionInfo *ci =
        MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    /* For simplicity return a static placeholder if sockaddr parsing needed */
    return ci ? "client" : "unknown";
}

/* Validate Authorization header → fill sess.  Returns true if valid. */
static bool require_auth(struct MHD_Connection *conn,
                          const char *action,
                          Session *sess,
                          enum MHD_Result *resp_out)
{
    const char *token =
        MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Authorization");
    if (!token || !validate_session(token, sess)) {
        *resp_out = json_error(conn, MHD_HTTP_UNAUTHORIZED, "unauthorized");
        return false;
    }
    if (action && !check_permission(sess->role, action)) {
        char msg[128];
        char detail[256];
        snprintf(msg, sizeof(msg),
                 "forbidden — role '%s' cannot '%s'", sess->role, action);

        /* ── LOG: Unauthorized action attempt ── */
        { char ts[32]; get_timestamp(ts, sizeof(ts));   /* FIX: moved ts out of snprintf arg — GNU stmt-expr returned dangling ptr */
        snprintf(detail, sizeof(detail),
                 "[%s] Action=%s Role=%s | IP=%s",
                 ts, action, sess->role,
                 client_ip(conn)); }
        log_user_activity(sess->user, "PERMISSION_DENIED", detail);

        activity_log_event(sess->user, "access_denied", msg, client_ip(conn));
        *resp_out = json_error(conn, MHD_HTTP_FORBIDDEN, msg);
        return false;
    }
    return true;
}

/* ── Request body accumulator ───────────────────────────────────── */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} BodyBuf;

static void body_append(BodyBuf *b, const char *chunk, size_t sz)
{
    if (b->len + sz + 1 > b->cap) {
        b->cap = b->len + sz + 1 + 1024;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, chunk, sz);
    b->len += sz;
    b->data[b->len] = '\0';
}

/* ── Route handlers ─────────────────────────────────────────────── */

/* GET / — serve auth_test.html */
static enum MHD_Result handle_index(struct MHD_Connection *conn)
{
    FILE *f = fopen(HTML_FILE, "rb");
    if (!f) return json_error(conn, MHD_HTTP_NOT_FOUND, "UI file not found");

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    fclose(f);
    buf[sz] = '\0';

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(sz, buf, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "text/html");
    enum MHD_Result r = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return r;
}

/* POST /login */
static enum MHD_Result handle_login(struct MHD_Connection *conn,
                                     const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j        = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));
    const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));

    if (!username || !password || !*username || !*password) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "username and password required");
    }

    UserRecord user;
    if (!get_user(username, &user)) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Login attempt for unknown user ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Unknown username attempted login | IP=%s", ts, ip);
        log_user_activity("anonymous", "LOGIN_UNKNOWN_USER", detail);

        activity_log_event("anonymous", "login_failed",
                  "Login attempt for unknown user", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "invalid username or password");
    }

    if (!verify_password(password, user.password_hash)) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Wrong password ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Wrong password entered | IP=%s", ts, ip);
        log_user_activity(username, "LOGIN_WRONG_PASSWORD", detail);

        activity_log_event(username, "login_failed", "Wrong password", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "invalid username or password");
    }

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Password OK, awaiting 2FA ── */
    snprintf(detail, sizeof(detail),
             "[%s] Password verified, proceeding to 2FA | IP=%s", ts, ip);
    log_user_activity(username, "LOGIN_PASSWORD_OK", detail);

    activity_log_event(username, "login_password_ok",
              "Password verified, awaiting 2FA", ip);

    PendingSession ps = {0};
    snprintf(ps.role, sizeof(ps.role), "%s", user.role);  /* FIX: strncpy truncation warning — snprintf always null-terminates */
    ps.expires_at = time(NULL) + OTP_TTL_SEC;
    pending_set(username, &ps);

    int has_email = user.email[0] != '\0';
    int has_totp  = user.totp_secret[0] != '\0';

    char hint[8] = "";
    if (has_email && strlen(user.email) > 3) {
        strncpy(hint, user.email, 3);
        strcat(hint, "***");
    }

    char resp_buf[512];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"step\":\"choose_2fa\",\"username\":\"%s\","
             "\"has_email\":%s,\"has_totp\":%s,"
             "\"email_hint\":\"%s\","
             "\"totp_label\":\"Microsoft Authenticator (OTP from app)\"}",
             username,
             has_email ? "true" : "false",
             has_totp  ? "true" : "false",
             hint);

    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp_buf);
}

/* POST /send-email-otp */
static enum MHD_Result handle_send_email_otp(struct MHD_Connection *conn,
                                              const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));

    PendingSession ps;
    if (!username || !pending_get(username, &ps)) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "session expired, please login again");
    }

    UserRecord user;
    if (!get_user(username, &user) || !user.email[0]) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "no email registered for this user");
    }

    generate_otp(ps.otp);
    ps.expires_at = time(NULL) + OTP_TTL_SEC;
    pending_set(username, &ps);

    const char *otp_recipients[1] = { user.email };
    bool sent = email_otp_send(&g_email_cfg, otp_recipients, 1, ps.otp);
    if (!sent) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: OTP email send failed ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Failed to send email OTP to %s | IP=%s",
                 ts, user.email, ip);
        log_user_activity(username, "OTP_EMAIL_SEND_FAILED", detail);

        activity_log_event(username, "otp_send_failed", "Failed to send email OTP", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "failed to send OTP email");
    }

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: OTP sent successfully ── */
    snprintf(detail, sizeof(detail),
             "[%s] Email OTP sent to %s | IP=%s", ts, user.email, ip);
    log_user_activity(username, "OTP_EMAIL_SENT", detail);

    activity_log_event(username, "otp_sent", "Email OTP sent", ip);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"step\":\"otp_sent\",\"message\":\"OTP sent to %s\"}",
             user.email);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /verify-otp */
static enum MHD_Result handle_verify_otp(struct MHD_Connection *conn,
                                          const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username    = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));
    const char *otp_entered = cJSON_GetStringValue(cJSON_GetObjectItem(j, "otp"));

    PendingSession ps;
    if (!username || !pending_get(username, &ps)) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "session expired, please login again");
    }

    if (!otp_entered || strcmp(otp_entered, ps.otp) != 0) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Wrong email OTP ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Invalid email OTP entered | IP=%s", ts, ip);
        log_user_activity(username, "OTP_EMAIL_VERIFY_FAILED", detail);

        activity_log_event(username, "otp_verify_failed", "Invalid email OTP", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "invalid or expired OTP");
    }

    pending_delete(username);
    char token[TOKEN_LEN];
    create_session(username, ps.role, token);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Login complete via email OTP ── */
    snprintf(detail, sizeof(detail),
             "[%s] Login successful via Email OTP | Role=%s | IP=%s",
             ts, ps.role, ip);
    log_user_activity(username, "LOGIN_SUCCESS_EMAIL_OTP", detail);

    activity_log_event(username, "login", "Successful login via email OTP", ip);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"token\":\"%s\",\"role\":\"%s\"}", token, ps.role);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /verify-totp */
static enum MHD_Result handle_verify_totp(struct MHD_Connection *conn,
                                           const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));
    const char *code     = cJSON_GetStringValue(cJSON_GetObjectItem(j, "code"));

    PendingSession ps;
    if (!username || !pending_get(username, &ps)) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "session expired, please login again");
    }

    UserRecord user;
    if (!get_user(username, &user) || !user.totp_secret[0]) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "no authenticator configured for this user");
    }

    if (!code || !verify_totp(user.totp_secret, code)) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Wrong TOTP code ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Invalid MS Authenticator code entered | IP=%s", ts, ip);
        log_user_activity(username, "TOTP_VERIFY_FAILED", detail);

        activity_log_event(username, "totp_verify_failed", "Invalid TOTP code", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "invalid authenticator code");
    }

    pending_delete(username);
    char token[TOKEN_LEN];
    create_session(username, ps.role, token);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Login complete via TOTP ── */
    snprintf(detail, sizeof(detail),
             "[%s] Login successful via MS Authenticator | Role=%s | IP=%s",
             ts, ps.role, ip);
    log_user_activity(username, "LOGIN_SUCCESS_TOTP", detail);

    activity_log_event(username, "login",
              "Successful login via TOTP authenticator", ip);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"token\":\"%s\",\"role\":\"%s\"}", token, ps.role);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /forgot-password */
static enum MHD_Result handle_forgot_password(struct MHD_Connection *conn,
                                               const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));

    if (!username || !*username) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST, "username required");
    }

    UserRecord user;
    if (!get_user(username, &user)) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Forgot password for unknown user ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Forgot password attempted for unknown user | IP=%s",
                 ts, ip);
        log_user_activity("anonymous", "FORGOT_PASSWORD_UNKNOWN_USER", detail);

        activity_log_event("anonymous", "forgot_password_attempt",
                  "Forgot password for unknown user", ip);
        cJSON_Delete(j);
        return send_json(conn, MHD_HTTP_OK,
                         "{\"has_email\":false,\"has_totp\":false,"
                         "\"message\":\"If that username exists, options will appear\"}");
    }

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Forgot password initiated ── */
    snprintf(detail, sizeof(detail),
             "[%s] Password reset flow initiated | IP=%s", ts, ip);
    log_user_activity(username, "FORGOT_PASSWORD_REQUESTED", detail);

    activity_log_event(username, "forgot_password_requested",
              "Forgot password flow initiated", ip);

    int has_email = user.email[0] != '\0';
    int has_totp  = user.totp_secret[0] != '\0';

    char hint[8] = "";
    if (has_email && strlen(user.email) > 3) {
        strncpy(hint, user.email, 3);
        strcat(hint, "***");
    }

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"username\":\"%s\",\"has_email\":%s,"
             "\"has_totp\":%s,\"email_hint\":\"%s\"}",
             username,
             has_email ? "true" : "false",
             has_totp  ? "true" : "false",
             hint);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /forgot-send-email */
static enum MHD_Result handle_forgot_send_email(struct MHD_Connection *conn,
                                                  const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));

    UserRecord user;
    if (!username || !get_user(username, &user) || !user.email[0]) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "no email registered for this user");
    }

    PendingSession ps = {0};
    generate_otp(ps.otp);
    snprintf(ps.role,   sizeof(ps.role),   "%s", user.role);  /* FIX: strncpy truncation */
    snprintf(ps.action, sizeof(ps.action), "%s", "reset_password");  /* FIX: strncpy truncation */
    ps.expires_at = time(NULL) + OTP_TTL_SEC;
    pending_set(username, &ps);

    const char *otp_recipients[1] = { user.email };
    bool sent = email_otp_send(&g_email_cfg, otp_recipients, 1, ps.otp);
    if (!sent) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Reset OTP email failed ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Failed to send password reset OTP to %s | IP=%s",
                 ts, user.email, ip);
        log_user_activity(username, "RESET_OTP_SEND_FAILED", detail);

        activity_log_event(username, "reset_otp_send_failed",
                  "Failed to send reset OTP", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "failed to send reset email");
    }

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Reset OTP sent ── */
    snprintf(detail, sizeof(detail),
             "[%s] Password reset OTP sent to %s | IP=%s",
             ts, user.email, ip);
    log_user_activity(username, "RESET_OTP_SENT", detail);

    activity_log_event(username, "reset_otp_sent", "Password reset OTP sent", ip);
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"step\":\"otp_sent\",\"message\":\"Reset OTP sent to %s\"}",
             user.email);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /forgot-verify-otp */
static enum MHD_Result handle_forgot_verify_otp(struct MHD_Connection *conn,
                                                  const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username    = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));
    const char *otp_entered = cJSON_GetStringValue(cJSON_GetObjectItem(j, "otp"));

    PendingSession ps;
    if (!username || !pending_get(username, &ps)
        || strcmp(ps.action, "reset_password") != 0) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "session expired, please start again");
    }

    if (!otp_entered || strcmp(otp_entered, ps.otp) != 0) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Wrong reset OTP ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Invalid password reset OTP entered | IP=%s", ts, ip);
        log_user_activity(username, "RESET_OTP_VERIFY_FAILED", detail);

        activity_log_event(username, "reset_otp_verify_failed",
                  "Invalid reset OTP entered", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "invalid or expired OTP");
    }

    ps.otp_verified = true;
    pending_set(username, &ps);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Reset OTP verified ── */
    snprintf(detail, sizeof(detail),
             "[%s] Password reset OTP verified successfully | IP=%s", ts, ip);
    log_user_activity(username, "RESET_OTP_VERIFIED", detail);

    activity_log_event(username, "reset_otp_verified",
              "Password reset OTP verified", ip);

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"step\":\"verified\",\"username\":\"%s\"}", username);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /forgot-verify-totp */
static enum MHD_Result handle_forgot_verify_totp(struct MHD_Connection *conn,
                                                   const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));
    const char *code     = cJSON_GetStringValue(cJSON_GetObjectItem(j, "code"));

    UserRecord user;
    if (!username || !get_user(username, &user) || !user.totp_secret[0]) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "no authenticator configured for this user");
    }

    if (!code || !verify_totp(user.totp_secret, code)) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Wrong TOTP for reset ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Invalid TOTP code during password reset | IP=%s", ts, ip);
        log_user_activity(username, "RESET_TOTP_VERIFY_FAILED", detail);

        activity_log_event(username, "reset_totp_verify_failed",
                  "Invalid TOTP code during password reset", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "invalid authenticator code");
    }

    PendingSession ps = {0};
    snprintf(ps.role,   sizeof(ps.role),   "%s", user.role);  /* FIX: strncpy truncation */
    snprintf(ps.action, sizeof(ps.action), "%s", "reset_password");  /* FIX: strncpy truncation */
    ps.totp_verified = true;
    ps.expires_at    = time(NULL) + OTP_TTL_SEC;
    pending_set(username, &ps);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: TOTP verified for reset ── */
    snprintf(detail, sizeof(detail),
             "[%s] MS Authenticator code verified for password reset | IP=%s",
             ts, ip);
    log_user_activity(username, "RESET_TOTP_VERIFIED", detail);

    activity_log_event(username, "reset_totp_verified",
              "TOTP verified for password reset", ip);

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"step\":\"verified\",\"username\":\"%s\"}", username);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /reset-password */
static enum MHD_Result handle_reset_password(struct MHD_Connection *conn,
                                              const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username     = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));
    const char *new_password = cJSON_GetStringValue(cJSON_GetObjectItem(j, "new_password"));

    PendingSession ps;
    if (!username || !pending_get(username, &ps)
        || strcmp(ps.action, "reset_password") != 0) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "session expired, please start again");
    }

    if (!ps.otp_verified && !ps.totp_verified) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_UNAUTHORIZED,
                          "please verify OTP first");
    }

    char errors[PASSWORD_MAX_ERRORS][PASSWORD_ERROR_LEN];
    int  error_count = 0;
    if (!password_policy_check(new_password ? new_password : "",
                                errors, &error_count, PASSWORD_MAX_ERRORS)) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Password reset failed policy check ── */
        snprintf(detail, sizeof(detail),
                 "[%s] New password failed policy check: %s | IP=%s",
                 ts, errors[0], ip);
        log_user_activity(username, "PASSWORD_RESET_POLICY_FAIL", detail);

        activity_log_event(username, "password_reset_failed", errors[0], ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST, errors[0]);
    }

    char new_hash[BCRYPT_HASH_LEN];
    hash_password(new_password, new_hash);
    update_password_hash(username, new_hash);
    pending_delete(username);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Password reset success ── */
    snprintf(detail, sizeof(detail),
             "[%s] Password successfully reset | IP=%s", ts, ip);
    log_user_activity(username, "PASSWORD_RESET_SUCCESS", detail);

    printf("  [Reset] password updated  user=%s\n", username);
    activity_log_event(username, "password_reset",
              "Password successfully reset", ip);

    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK,
                     "{\"message\":\"Password updated successfully\"}");
}

/* GET /secure-data */
static enum MHD_Result handle_secure_data(struct MHD_Connection *conn,
                                           const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "secure-data", &sess, &err)) return err;

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Secure data accessed ── */
    snprintf(detail, sizeof(detail),
             "[%s] Secure data accessed | Role=%s | IP=%s",
             ts, sess.role, ip);
    log_user_activity(sess.user, "SECURE_DATA_ACCESSED", detail);

    activity_log_event(sess.user, "secure_data_access",
              "Secure data accessed", ip);

    const char *perms[20];
    int np = get_permissions(sess.role, perms, 20);
    char perm_arr[512] = "[";
    for (int i = 0; i < np; i++) {
        if (i) strcat(perm_arr, ",");
        strcat(perm_arr, "\"");
        strcat(perm_arr, perms[i]);
        strcat(perm_arr, "\"");
    }
    strcat(perm_arr, "]");

    char resp[RESP_BUF_SIZE];
    snprintf(resp, sizeof(resp),
             "{\"message\":\"secure data access granted\","
             "\"role\":\"%s\",\"user\":\"%s\","
             "\"permissions\":%s,"
             "\"data\":{\"camera_count\":5,"
             "\"server_ip\":\"192.168.1.160\","
             "\"stream_port\":8554}}",
             sess.role, sess.user, perm_arr);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /start-stream */
static enum MHD_Result handle_start_stream(struct MHD_Connection *conn,
                                            const char *body, const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "start-stream", &sess, &err)) return err;

    cJSON *j = cJSON_Parse(body ? body : "{}");
    cJSON *cid_item = cJSON_GetObjectItem(j, "camera_id");
    int camera_id = cid_item ? cid_item->valueint : 0;
    cJSON_Delete(j);

    char device[32];
    snprintf(device, sizeof(device), "/dev/video%d", camera_id);

    /* ── Check if device is a real capture-capable V4L2 camera ── */
    {
        int fd = open(device, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            get_timestamp(ts, sizeof(ts));
            snprintf(detail, sizeof(detail),
                     "[%s] Stream start FAILED — cannot open %s | IP=%s",
                     ts, device, ip);
            log_user_activity(sess.user, "STREAM_DEVICE_NOT_FOUND", detail);
            activity_log_event(sess.user, "stream_start_failed",
                      "Camera device not found", ip);
            char err_resp[128];
            snprintf(err_resp, sizeof(err_resp),
                     "{\"error\":\"Camera %d (/dev/video%d) is not connected\"}",
                     camera_id, camera_id);
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err_resp);
        }
        struct v4l2_capability cap;
        int cap_ok = (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
        int is_capture = cap_ok &&
                         ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
                          (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE));
        close(fd);
        if (!is_capture) {
            get_timestamp(ts, sizeof(ts));
            snprintf(detail, sizeof(detail),
                     "[%s] Stream start FAILED — %s is not a capture device | IP=%s",
                     ts, device, ip);
            log_user_activity(sess.user, "STREAM_DEVICE_NOT_CAPTURE", detail);
            activity_log_event(sess.user, "stream_start_failed",
                      "Not a capture device", ip);
            char err_resp[128];
            snprintf(err_resp, sizeof(err_resp),
                     "{\"error\":\"Camera %d (/dev/video%d) is not a video capture device\"}",
                     camera_id, camera_id);
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err_resp);
        }
    }

    /* Build device argument string for GStreamer */
    char dev_arg[48];
    snprintf(dev_arg, sizeof(dev_arg), "device=/dev/video%d", camera_id);

    /* ── stream_mutex: protect stream_pid from concurrent access ── */
    pthread_mutex_lock(&stream_mutex);

    /* Check if already running */
    if (stream_pid > 0 && waitpid(stream_pid, NULL, WNOHANG) == 0) {
        pthread_mutex_unlock(&stream_mutex);

        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Stream already running ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Stream start requested but already running | Camera=%s | IP=%s",
                 ts, device, ip);
        log_user_activity(sess.user, "STREAM_ALREADY_RUNNING", detail);

        activity_log_event(sess.user, "stream_start_skipped",
                  "Stream already running", ip);
        return send_json(conn, MHD_HTTP_OK,
                         "{\"message\":\"stream already running\"}");
    }

    /* Launch gstreamer pipeline as child process */
    pid_t new_pid = fork();
    if (new_pid == 0) {
        /* HLS pipeline — writes segment*.ts + playlist.m3u8 into the
         * https_Cported working directory so http_server can serve them. */
        execlp("gst-launch-1.0", "gst-launch-1.0", "-e",
               "v4l2src", dev_arg, "io-mode=dmabuf",
               "!", "video/x-raw,width=1920,height=1080,framerate=30/1,format=NV12",
               "!", "v4l2h264enc",
                     "extra-controls=controls,video_bitrate=2000000,h264_i_frame_period=30;",
               "!", "h264parse",
               "!", "mpegtsmux",
               "!", "hlssink",
                     "location=/root/https_Cported/segment%05d.ts",
                     "playlist-location=/root/https_Cported/playlist.m3u8",
                     "playlist-root=https://192.168.1.160:8443",
                     "target-duration=1",
                     "max-files=5",
               NULL);
        _exit(1);
    }
    if (new_pid < 0) {
        pthread_mutex_unlock(&stream_mutex);

        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Stream start failed ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Stream start FAILED (fork error) | Camera=%s | IP=%s",
                 ts, device, ip);
        log_user_activity(sess.user, "STREAM_START_FAILED", detail);

        activity_log_event(sess.user, "stream_start_failed",
                  "fork() failed", ip);
        return json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "failed to start stream");
    }
    stream_pid = new_pid;
    pthread_mutex_unlock(&stream_mutex);
    /* ─────────────────────────────────────────────────────────── */

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Stream started successfully ── */
    snprintf(detail, sizeof(detail),
             "[%s] Camera stream started | Camera=%s | PID=%d | IP=%s",
             ts, device, stream_pid, ip);
    log_user_activity(sess.user, "STREAM_STARTED", detail);

    printf("  [Stream] started  device=%s  pid=%d  by=%s\n",
           device, stream_pid, sess.user);
    activity_log_event(sess.user, "stream_start", "Stream started", ip);

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"message\":\"stream started on %s\",\"camera_id\":%d}",
             device, camera_id);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /stop-stream */
static enum MHD_Result handle_stop_stream(struct MHD_Connection *conn,
                                           const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "stop-stream", &sess, &err)) return err;

    /* ── stream_mutex: protect stream_pid from concurrent access ── */
    pthread_mutex_lock(&stream_mutex);

    if (stream_pid <= 0 || waitpid(stream_pid, NULL, WNOHANG) != 0) {
        stream_pid = -1;
        pthread_mutex_unlock(&stream_mutex);

        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Stop requested but not running ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Stop stream requested but stream not running | IP=%s",
                 ts, ip);
        log_user_activity(sess.user, "STREAM_STOP_NOT_RUNNING", detail);

        return send_json(conn, MHD_HTTP_OK,
                         "{\"message\":\"stream not running\"}");
    }

    kill(stream_pid, SIGTERM);
    waitpid(stream_pid, NULL, 0);
    stream_pid = -1;
    pthread_mutex_unlock(&stream_mutex);
    /* ─────────────────────────────────────────────────────────── */

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Stream stopped ── */
    snprintf(detail, sizeof(detail),
             "[%s] Camera stream stopped | IP=%s", ts, ip);
    log_user_activity(sess.user, "STREAM_STOPPED", detail);

    printf("  [Stream] stopped  by=%s\n", sess.user);
    activity_log_event(sess.user, "stream_stop", "Stream stopped", ip);
    return send_json(conn, MHD_HTTP_OK, "{\"message\":\"stream stopped\"}");
}

/* POST /device/reboot */
static enum MHD_Result handle_device_reboot(struct MHD_Connection *conn,
                                             const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "device:reboot", &sess, &err)) return err;

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Device reboot triggered ── */
    snprintf(detail, sizeof(detail),
             "[%s] Device REBOOT triggered | Role=%s | IP=%s",
             ts, sess.role, ip);
    log_user_activity(sess.user, "DEVICE_REBOOT", detail);

    char result[512];
    device_reboot(&sess, ip, result, sizeof(result));
    return send_json(conn, MHD_HTTP_OK, result);
}

/* GET /device/status */
static enum MHD_Result handle_device_status(struct MHD_Connection *conn,
                                             const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "device:status", &sess, &err)) return err;

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Device status checked ── */
    snprintf(detail, sizeof(detail),
             "[%s] Device status checked | Role=%s | IP=%s",
             ts, sess.role, ip);
    log_user_activity(sess.user, "DEVICE_STATUS_CHECKED", detail);

    char result[RESP_BUF_SIZE];
    device_status(&sess, ip, result, sizeof(result));
    return send_json(conn, MHD_HTTP_OK, result);
}

/* POST /device/network-reset */
static enum MHD_Result handle_network_reset(struct MHD_Connection *conn,
                                             const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "device:network-reset", &sess, &err)) return err;

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Network reset triggered ── */
    snprintf(detail, sizeof(detail),
             "[%s] Network RESET triggered | Role=%s | IP=%s",
             ts, sess.role, ip);
    log_user_activity(sess.user, "NETWORK_RESET", detail);

    char result[256];
    network_reset(&sess, ip, result, sizeof(result));
    return send_json(conn, MHD_HTTP_OK, result);
}

/* POST /device/config-reset */
static enum MHD_Result handle_config_reset(struct MHD_Connection *conn,
                                            const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "device:config-reset", &sess, &err)) return err;

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Config reset triggered ── */
    snprintf(detail, sizeof(detail),
             "[%s] Config RESET triggered | Role=%s | IP=%s",
             ts, sess.role, ip);
    log_user_activity(sess.user, "CONFIG_RESET", detail);

    char result[256];
    config_reset(&sess, ip, result, sizeof(result));
    return send_json(conn, MHD_HTTP_OK, result);
}

/* POST /device/factory-reset */
static enum MHD_Result handle_factory_reset(struct MHD_Connection *conn,
                                             const char *body, const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "device:factory-reset", &sess, &err)) return err;

    cJSON *j = cJSON_Parse(body ? body : "{}");
    cJSON *conf_item = cJSON_GetObjectItem(j, "confirm");
    int confirm = conf_item && cJSON_IsTrue(conf_item);
    cJSON_Delete(j);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: Factory reset triggered ── */
    snprintf(detail, sizeof(detail),
             "[%s] FACTORY RESET triggered (confirm=%s) | Role=%s | IP=%s",
             ts, confirm ? "true" : "false", sess.role, ip);
    log_user_activity(sess.user, "FACTORY_RESET", detail);

    char result[256];
    int rc = factory_reset(&sess, ip, confirm, result, sizeof(result));
    if (rc == -2)
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "confirm: true is required for factory reset");
    return send_json(conn, MHD_HTTP_OK, result);
}

/* POST /add-user */
static enum MHD_Result handle_add_user(struct MHD_Connection *conn,
                                        const char *body, const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "manage:add-user", &sess, &err)) return err;

    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));
    const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
    const char *email    = cJSON_GetStringValue(cJSON_GetObjectItem(j, "email"));
    const char *new_role = cJSON_GetStringValue(cJSON_GetObjectItem(j, "role"));
    if (!new_role) new_role = "viewer";

    if (!username || !*username || !password || !*password || !email || !*email) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "username, password and email are required");
    }

    /* Validate role */
    int role_ok = 0;
    for (int i = 0; USER_ROLES[i]; i++)
        if (strcmp(new_role, USER_ROLES[i]) == 0) { role_ok = 1; break; }
    if (!role_ok) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "invalid role — must be admin, operator, or viewer");
    }

    char errors[PASSWORD_MAX_ERRORS][PASSWORD_ERROR_LEN];
    int  error_count = 0;
    if (!password_policy_check(password, errors, &error_count,
                                PASSWORD_MAX_ERRORS)) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST, errors[0]);
    }

    if (user_exists(username)) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: Add user failed — already exists ── */
        snprintf(detail, sizeof(detail),
                 "[%s] Add user FAILED — user '%s' already exists | IP=%s",
                 ts, username, ip);
        log_user_activity(sess.user, "ADD_USER_FAILED_EXISTS", detail);

        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_CONFLICT, "user already exists");
    }

    char totp_secret[TOTP_SECRET_MAX];
    char qr_path[256], totp_uri[URI_MAX];
    generate_totp_secret(totp_secret);
    generate_qr_code(username, totp_secret, QR_DIR, qr_path, totp_uri);

    if (*email) {
        const char *totp_recipients_email[1] = { email };
        bool sent = email_otp_send_totp_setup(
            &g_email_cfg, totp_recipients_email, 1, username, qr_path, totp_uri);
        printf("  [AddUser] MS Authenticator setup email %s → %s\n",
               sent ? "sent" : "FAILED", email);
    }

    char hash[BCRYPT_HASH_LEN];
    hash_password(password, hash);
    insert_user(username, hash, new_role, email, totp_secret);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: User added successfully ── */
    snprintf(detail, sizeof(detail),
             "[%s] New user '%s' added with role='%s' | By=%s | IP=%s",
             ts, username, new_role, sess.user, ip);
    log_user_activity(sess.user, "USER_ADDED", detail);

    printf("  [AddUser] created  user=%s  role=%s  by=%s\n",
           username, new_role, sess.user);
    activity_log_event(sess.user, "user_created",
              "Admin created new user", ip);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"message\":\"user '%s' created with role '%s'\"}",
             username, new_role);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /resend-totp-setup */
static enum MHD_Result handle_resend_totp_setup(struct MHD_Connection *conn,
                                                  const char *body, const char *ip)
{
    char ts[32], detail[256];
    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));

    if (!username || !*username) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST, "username required");
    }

    /* No pending session required: this endpoint is reachable from both the
     * normal login TOTP flow AND the forgot-password TOTP flow (where no
     * pending session exists yet).  Identity is validated implicitly — we
     * only send the email to the address already stored in the DB for this
     * username, so there is nothing an attacker gains by calling this. */

    UserRecord user;
    if (!get_user(username, &user)) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_NOT_FOUND, "user not found");
    }
    if (!user.email[0]) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST,
                          "no email registered for this user");
    }
    char qr_path[256], totp_uri[URI_MAX];

    if (!user.totp_secret[0]) {
        /* User has no TOTP secret yet — this happens when they reach the
         * forgot-password TOTP path without ever completing initial setup.
         * Generate a fresh secret, store it, then fall through to send the
         * setup email normally. */
        generate_totp_secret(user.totp_secret);
        update_totp_secret(username, user.totp_secret);

        get_timestamp(ts, sizeof(ts));
        snprintf(detail, sizeof(detail),
                 "[%s] New TOTP secret auto-generated via setup-resend | IP=%s",
                 ts, ip);
        log_user_activity(username, "TOTP_SECRET_GENERATED", detail);
        activity_log_event(username, "totp_secret_generated",
                  "New TOTP secret generated on setup resend", ip);
    }

    generate_qr_code(username, user.totp_secret, QR_DIR, qr_path, totp_uri);

    const char *totp_recipients_user[1] = { user.email };
    bool sent = email_otp_send_totp_setup(
        &g_email_cfg, totp_recipients_user, 1, username, qr_path, totp_uri);
    if (!sent) {
        get_timestamp(ts, sizeof(ts));

        /* ── LOG: TOTP setup resend failed ── */
        snprintf(detail, sizeof(detail),
                 "[%s] TOTP setup email resend FAILED to %s | IP=%s",
                 ts, user.email, ip);
        log_user_activity(username, "TOTP_SETUP_RESEND_FAILED", detail);

        activity_log_event(username, "totp_setup_resend_failed",
                  "Failed to resend TOTP setup email", ip);
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "failed to send setup email");
    }

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: TOTP setup resent ── */
    snprintf(detail, sizeof(detail),
             "[%s] MS Authenticator setup email resent to %s | IP=%s",
             ts, user.email, ip);
    log_user_activity(username, "TOTP_SETUP_RESENT", detail);

    activity_log_event(username, "totp_setup_resent",
              "MS Authenticator setup email resent", ip);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"message\":\"Setup email sent to %s\"}", user.email);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /remove-user  (admin only) */
static enum MHD_Result handle_remove_user(struct MHD_Connection *conn,
                                           const char *body, const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    enum MHD_Result err;
    if (!require_auth(conn, "manage:remove-user", &sess, &err)) return err;

    cJSON *j = cJSON_Parse(body);
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(j, "username"));

    if (!username || !*username) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_BAD_REQUEST, "username required");
    }

    /* Prevent an admin from deleting their own account */
    if (strcmp(username, sess.user) == 0) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_FORBIDDEN,
                          "cannot remove your own account");
    }

    if (!user_exists(username)) {
        cJSON_Delete(j);
        return json_error(conn, MHD_HTTP_NOT_FOUND, "user not found");
    }

    delete_user(username);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: User removed ── */
    snprintf(detail, sizeof(detail),
             "[%s] User '%s' REMOVED | By=%s | IP=%s",
             ts, username, sess.user, ip);
    log_user_activity(sess.user, "USER_REMOVED", detail);

    printf("  [RemoveUser] deleted  user=%s  by=%s\n", username, sess.user);
    activity_log_event(sess.user, "user_removed", username, ip);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"message\":\"user '%s' removed successfully\"}", username);
    cJSON_Delete(j);
    return send_json(conn, MHD_HTTP_OK, resp);
}

/* POST /logout */
static enum MHD_Result handle_logout(struct MHD_Connection *conn,
                                      const char *ip)
{
    char ts[32], detail[256];
    Session sess;
    const char *token =
        MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Authorization");
    if (!token || !validate_session(token, &sess)) {
        return json_error(conn, MHD_HTTP_UNAUTHORIZED, "unauthorized");
    }

    delete_session(token);

    get_timestamp(ts, sizeof(ts));

    /* ── LOG: User logged out ── */
    snprintf(detail, sizeof(detail),
             "[%s] User logged out | Role=%s | IP=%s",
             ts, sess.role, ip);
    log_user_activity(sess.user, "LOGOUT", detail);

    printf("  [Logout] user=%s  ip=%s\n", sess.user, ip);
    activity_log_event(sess.user, "logout", "session terminated", ip);

    return send_json(conn, MHD_HTTP_OK, "{\"message\":\"logged out successfully\"}");
}

/* ── Main request dispatcher ─────────────────────────────────────── */

typedef struct {
    BodyBuf body;
} ConnCtx;

static enum MHD_Result request_handler(void *cls,
                                        struct MHD_Connection *conn,
                                        const char *url,
                                        const char *method,
                                        const char *version,
                                        const char *upload_data,
                                        size_t *upload_data_size,
                                        void **con_cls)
{
    (void)cls; (void)version;

    /* First call: allocate context */
    if (!*con_cls) {
        ConnCtx *ctx = calloc(1, sizeof(ConnCtx));
        ctx->body.data = malloc(1);
        ctx->body.data[0] = '\0';
        ctx->body.cap = 1;
        *con_cls = ctx;
        return MHD_YES;
    }

    ConnCtx *ctx = *con_cls;

    /* Accumulate POST body */
    if (*upload_data_size) {
        body_append(&ctx->body, upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    const char *body = ctx->body.data ? ctx->body.data : "";
    const char *ip   = client_ip(conn);

    /* Handle OPTIONS preflight */
    if (strcmp(method, "OPTIONS") == 0) {
        struct MHD_Response *r =
            MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(r, "Access-Control-Allow-Origin",  "*");
        MHD_add_response_header(r, "Access-Control-Allow-Methods",
                                "GET, POST, OPTIONS");
        MHD_add_response_header(r, "Access-Control-Allow-Headers",
                                "Content-Type, Authorization");
        enum MHD_Result res = MHD_queue_response(conn, MHD_HTTP_OK, r);
        MHD_destroy_response(r);
        return res;
    }

    enum MHD_Result result = MHD_NO;

    if (strcmp(url, "/") == 0 && strcmp(method, "GET") == 0)
        result = handle_index(conn);
    else if (strcmp(url, "/login") == 0 && strcmp(method, "POST") == 0)
        result = handle_login(conn, body, ip);
    else if (strcmp(url, "/send-email-otp") == 0 && strcmp(method, "POST") == 0)
        result = handle_send_email_otp(conn, body, ip);
    else if (strcmp(url, "/verify-otp") == 0 && strcmp(method, "POST") == 0)
        result = handle_verify_otp(conn, body, ip);
    else if (strcmp(url, "/verify-totp") == 0 && strcmp(method, "POST") == 0)
        result = handle_verify_totp(conn, body, ip);
    else if (strcmp(url, "/resend-totp-setup") == 0 && strcmp(method, "POST") == 0)
        result = handle_resend_totp_setup(conn, body, ip);
    else if (strcmp(url, "/forgot-password") == 0 && strcmp(method, "POST") == 0)
        result = handle_forgot_password(conn, body, ip);
    else if (strcmp(url, "/forgot-send-email") == 0 && strcmp(method, "POST") == 0)
        result = handle_forgot_send_email(conn, body, ip);
    else if (strcmp(url, "/forgot-verify-otp") == 0 && strcmp(method, "POST") == 0)
        result = handle_forgot_verify_otp(conn, body, ip);
    else if (strcmp(url, "/forgot-verify-totp") == 0 && strcmp(method, "POST") == 0)
        result = handle_forgot_verify_totp(conn, body, ip);
    else if (strcmp(url, "/reset-password") == 0 && strcmp(method, "POST") == 0)
        result = handle_reset_password(conn, body, ip);
    else if (strcmp(url, "/secure-data") == 0 && strcmp(method, "GET") == 0)
        result = handle_secure_data(conn, ip);
    else if (strcmp(url, "/start-stream") == 0 && strcmp(method, "POST") == 0)
        result = handle_start_stream(conn, body, ip);
    else if (strcmp(url, "/stop-stream") == 0 && strcmp(method, "POST") == 0)
        result = handle_stop_stream(conn, ip);
    else if (strcmp(url, "/device/reboot") == 0 && strcmp(method, "POST") == 0)
        result = handle_device_reboot(conn, ip);
    else if (strcmp(url, "/device/status") == 0 && strcmp(method, "GET") == 0)
        result = handle_device_status(conn, ip);
    else if (strcmp(url, "/device/network-reset") == 0 && strcmp(method, "POST") == 0)
        result = handle_network_reset(conn, ip);
    else if (strcmp(url, "/device/config-reset") == 0 && strcmp(method, "POST") == 0)
        result = handle_config_reset(conn, ip);
    else if (strcmp(url, "/device/factory-reset") == 0 && strcmp(method, "POST") == 0)
        result = handle_factory_reset(conn, body, ip);
    else if (strcmp(url, "/add-user") == 0 && strcmp(method, "POST") == 0)
        result = handle_add_user(conn, body, ip);
    else if (strcmp(url, "/remove-user") == 0 && strcmp(method, "POST") == 0)
        result = handle_remove_user(conn, body, ip);
    else if (strcmp(url, "/logout") == 0 && strcmp(method, "POST") == 0)
        result = handle_logout(conn, ip);
    else
        result = json_error(conn, MHD_HTTP_NOT_FOUND, "not found");

    return result;
}

static void request_completed(void *cls,
                               struct MHD_Connection *conn,
                               void **con_cls,
                               enum MHD_RequestTerminationCode toe)
{
    (void)cls; (void)conn; (void)toe;
    ConnCtx *ctx = *con_cls;
    if (ctx) {
        free(ctx->body.data);
        free(ctx);
        *con_cls = NULL;
    }
}

/* ── Setup ───────────────────────────────────────────────────────── */

typedef struct {
    const char *username;
    const char *password;
    const char *role;
    const char *email;
} UserInit;

static const UserInit USERS[] = {
    { "admin",    "Admin@123",    "admin",    "admin7351@gmail.com" },
    { "operator", "Operator@123", "operator", "observer@gmail.com"  },
    { "viewer",   "Viewer@123",   "viewer",   "viewer@gmail.com"    },
    { "john",     "John@123!",    "admin",    "john@gmail.com"      },
    { NULL, NULL, NULL, NULL }
};

static void setup(void)
{
    char ts[32];
    get_timestamp(ts, sizeof(ts));

    printf("\n[Setup] Initializing...  [%s]\n", ts);
    printf("[Setup] Checking password policy...\n");

    for (int i = 0; USERS[i].username; i++) {
        char errors[PASSWORD_MAX_ERRORS][PASSWORD_ERROR_LEN];
        int  error_count = 0;
        if (!password_policy_check(USERS[i].password,
                                    errors, &error_count,
                                    PASSWORD_MAX_ERRORS)) {
            fprintf(stderr, "  ERROR: '%s' default password failed: %s\n",
                    USERS[i].username, errors[0]);
            exit(1);
        }
        printf("  [Setup] '%s' password OK\n", USERS[i].username);
    }

    init_db();
    mkdir(QR_DIR, 0755);

    mkdir(LOG_DIR, 0755);  /* create log dir for activity_logger */

    for (int i = 0; USERS[i].username; i++) {
        const UserInit *u = &USERS[i];
        if (user_exists(u->username)) {
            printf("  [Setup] '%s' already in DB — skipping\n", u->username);
            continue;
        }

        char totp_secret[TOTP_SECRET_MAX];
        char qr_path[256], totp_uri[URI_MAX];
        generate_totp_secret(totp_secret);
        generate_qr_code(u->username, totp_secret, QR_DIR, qr_path, totp_uri);
        printf("  [Setup] QR code → %s\n", qr_path);

        if (u->email && *u->email) {
            const char *totp_recipients_u[1] = { u->email };
            bool sent = email_otp_send_totp_setup(
                &g_email_cfg, totp_recipients_u, 1, u->username, qr_path, totp_uri);
            printf("  [Setup] MS Authenticator setup email %s → %s\n",
                   sent ? "sent" : "FAILED", u->email);
        }

        char hash[BCRYPT_HASH_LEN];
        hash_password(u->password, hash);
        insert_user(u->username, hash, u->role,
                    u->email ? u->email : "", totp_secret);

        activity_log_event(u->username, "user_created",
                  "New user created during server setup", NULL);
    }

    /* ── LOG: Server started ── */
    get_timestamp(ts, sizeof(ts));
    char detail[256];
    snprintf(detail, sizeof(detail),
             "[%s] SmartCamera server started on port %d", ts, SERVER_PORT);
    activity_log_event("system", "SERVER_START", detail, NULL);  /* FIX: LOG_INFO (int=6) was wrongly passed as const char* details; args were swapped */

    printf("\n[Setup] Users ready.\n");
    printf("[Setup] QR codes in: %s\n", QR_DIR);
    printf("[Setup] Activity log → %s/user_activity.log\n\n", LOG_DIR);
}

/* ── main ────────────────────────────────────────────────────────── */

int main(void)
{
    setup();

    /* Start plain HTTP daemon — TLS is handled by NGINX, not here */
    struct MHD_Daemon *daemon =
        MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
                         SERVER_PORT,
                         NULL, NULL,
                         &request_handler, NULL,
                         MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
                         MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr,
            "[Server] Failed to start HTTP server on port %d\n",
            SERVER_PORT);
        return 1;
    }

    printf("[Server] HTTP listening on http://127.0.0.1:%d\n", SERVER_PORT);
    printf("[Server] NGINX proxies https://0.0.0.0:443 → here\n");
    printf("[Server] Press Ctrl+C to stop.\n\n");

    /* Run forever */
    pause();

    MHD_stop_daemon(daemon);
    return 0;
}
