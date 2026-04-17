/*
 * core/http_server.c — HTTPS REST API Server (Merged v1+v2)
 * ===========================================================
 *
 * Routes registered in http_server_new():
 *
 *   [v2 original]
 *   GET  /                           Dashboard HTML
 *   POST /api/v1/auth/login          Phase-1 login (now starts 2FA)
 *   GET  /api/v1/health              Health check
 *   GET  /api/v1/stream/*            HLS streaming (auth: viewer)
 *   GET/DELETE /recordings/*         Static recording download
 *   GET  /api/v1/system/*            System status / log-level (main.c override)
 *   PATCH/POST/DELETE /api/v1/lenses/* Lens control (main.c override)
 *   GET/DELETE /api/v1/recordings    Recording list/delete (main.c override)
 *   POST /api/v1/ai/*                AI engine control
 *
 *   [v1 new]
 *   POST /api/v1/auth/verify-otp     Phase-2a: verify email OTP
 *   POST /api/v1/auth/verify-totp    Phase-2b: verify TOTP code
 *   POST /api/v1/auth/setup-totp     Generate/save TOTP secret + QR
 *   POST /api/v1/auth/logout         Revoke token
 *   POST /api/v1/auth/change-password
 *   POST /api/v1/auth/forgot-password
 *   POST /api/v1/auth/reset-password
 *   GET/POST/DELETE /api/v1/users    User management (admin only)
 *   GET/POST /api/v1/device/*        Device controls (v1 device_controls.c)
 */

#include "core/http_server.h"
#include "web_assets.h"
#include "../common/config.h"

/* v1 modules */
#include "../auth/activity_logger.h"
#include "../auth/db_manager.h"
#include "../device_controls.h"       /* device_reboot / status / resets */
#include "../auth/session_manager.h"  /* Session struct for device_controls */

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Server struct
 * ───────────────────────────────────────────────────────────────────────── */

struct _HTTPServer {
    SoupServer *soup;
    TokenAuth  *auth;
    int         port;
    gboolean    tls;
};

/* ─────────────────────────────────────────────────────────────────────────
 * JSON / error response helpers
 * ───────────────────────────────────────────────────────────────────────── */

void http_server_respond_json(SoupMessage *msg, int status, const char *json)
{
    soup_message_set_status(msg, status);
    soup_message_set_response(msg, "application/json",
                              SOUP_MEMORY_COPY, json, strlen(json));
}

void http_server_respond_error(SoupMessage *msg, int status, const char *err_msg)
{
    char *err = g_strdup_printf("{\"error\":\"%s\"}", err_msg);
    http_server_respond_json(msg, status, err);
    g_free(err);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Authorisation utilities
 * ───────────────────────────────────────────────────────────────────────── */

/* Extract token from Authorization header or ?token= query parameter. */
static const char *_extract_token(SoupMessage *msg, GHashTable *query)
{
    const char *hdr = soup_message_headers_get_one(msg->request_headers,
                                                    "Authorization");
    if (hdr && g_str_has_prefix(hdr, "Bearer "))
        return hdr + 7;
    if (query)
        return g_hash_table_lookup(query, "token");
    return NULL;
}

/* Role-hierarchy comparison: returns TRUE if actual_role satisfies required. */
static gboolean _role_satisfies(const char *actual, const char *required)
{
    if (!required) return TRUE;
    if (g_strcmp0(actual, "admin") == 0) return TRUE;
    if (g_strcmp0(actual, "operator") == 0)
        return (g_strcmp0(required, "operator") == 0 ||
                g_strcmp0(required, "viewer")   == 0);
    if (g_strcmp0(actual, "viewer") == 0)
        return (g_strcmp0(required, "viewer") == 0);
    return FALSE;
}

gboolean http_server_authorize(HTTPServer  *srv,
                               SoupMessage *msg,
                               GHashTable  *query,
                               const char  *required_role)
{
    char user[64], role[16];
    return http_server_authorize_ex(srv, msg, query, required_role,
                                    user, sizeof(user), role, sizeof(role));
}

gboolean http_server_authorize_ex(HTTPServer  *srv,
                                  SoupMessage *msg,
                                  GHashTable  *query,
                                  const char  *required_role,
                                  char        *out_user, int user_len,
                                  char        *out_role, int role_len)
{
    const char *token = _extract_token(msg, query);

    char user[64] = "", role[16] = "";
    if (!token_auth_validate(srv->auth, token, user, sizeof(user),
                             role, sizeof(role))) {
        http_server_respond_error(msg, 401, "Unauthorized");
        return FALSE;
    }

    if (!_role_satisfies(role, required_role)) {
        g_warning("[Auth] Access denied: user='%s' role='%s' required='%s'",
                  user, role, required_role);
        activity_log_event(user, "ACCESS_DENIED", required_role,
                           /* ip from headers: */ NULL);
        http_server_respond_error(msg, 403, "Forbidden");
        return FALSE;
    }

    g_debug("[HTTP] Authorised: user='%s' role='%s' required='%s'",
            user, role, required_role ? required_role : "none");

    if (out_user) g_strlcpy(out_user, user, user_len);
    if (out_role) g_strlcpy(out_role, role, role_len);
    return TRUE;
}

/* Build a Session struct for v1 device_controls from validated token info. */
static void _make_session(const char *user, const char *role, Session *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->user, user, USERNAME_MAX - 1);
    strncpy(out->role, role, ROLE_MAX - 1);
    out->active     = true;
    out->expires_at = time(NULL) + TOKEN_TTL_SEC;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Helper: get client IP from SoupClientContext
 * ───────────────────────────────────────────────────────────────────────── */
static const char *_client_ip(SoupClientContext *client)
{
    if (!client) return NULL;
    return soup_client_context_get_host(client);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Helper: read JSON body into a JsonObject
 * Returns NULL on failure.  Caller must g_object_unref(parser) and
 * soup_buffer_free(body) when done.
 * ───────────────────────────────────────────────────────────────────────── */
static JsonObject *_parse_body(SoupMessage *msg,
                               SoupBuffer **body_out,
                               JsonParser **parser_out)
{
    SoupBuffer *body = soup_message_body_flatten(msg->request_body);
    if (!body || body->length == 0) {
        if (body) soup_buffer_free(body);
        http_server_respond_error(msg, 400, "Request body required");
        return NULL;
    }
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, body->data, body->length, NULL)) {
        g_object_unref(p);
        soup_buffer_free(body);
        http_server_respond_error(msg, 400, "Invalid JSON");
        return NULL;
    }
    *body_out   = body;
    *parser_out = p;
    return json_node_get_object(json_parser_get_root(p));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * [v2 original] Base route handlers
 * ═════════════════════════════════════════════════════════════════════════ */

/* GET / — dashboard HTML */
static void _handle_root(SoupServer *server, SoupMessage *msg, const char *path,
                          GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)query; (void)client; (void)user_data;
    if (g_strcmp0(msg->method, "GET") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }
    soup_message_set_response(msg, "text/html", SOUP_MEMORY_STATIC,
                              (gchar *)dashboard_html, strlen(dashboard_html));
    soup_message_set_status(msg, 200);
}

/* GET /api/v1/health */
static void _handle_health(SoupServer *server, SoupMessage *msg, const char *path,
                            GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)query; (void)client; (void)user_data;
    if (g_strcmp0(msg->method, "GET") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }
    http_server_respond_json(msg, 200, "{\"status\":\"healthy\"}");
}

/* ─────────────────────────────────────────────────────────────────────────
 * POST /api/v1/auth/login  (now starts the 2FA flow)
 * ───────────────────────────────────────────────────────────────────────── */
static void _handle_login(SoupServer *server, SoupMessage *msg, const char *path,
                           GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)query;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    SoupBuffer *body; JsonParser *parser;
    JsonObject *obj = _parse_body(msg, &body, &parser);
    if (!obj) return;

    const char *username = json_object_get_string_member_with_default(obj, "username", "");
    const char *password = json_object_get_string_member_with_default(obj, "password", "");
    const char *ip       = _client_ip(client);

    char token_buf[AUTH_TOKEN_BUF] = "";
    AuthLoginResult result = token_auth_begin_login(srv->auth,
                                                    username, password,
                                                    ip,
                                                    token_buf, sizeof(token_buf));
    switch (result) {
    case AUTH_OK: {
        /* No email set — token issued directly */
        char *resp = g_strdup_printf("{\"token\":\"%s\"}", token_buf);
        http_server_respond_json(msg, 200, resp);
        g_free(resp);
        break;
    }
    case AUTH_OTP_REQUIRED: {
        char *resp = g_strdup_printf(
            "{\"status\":\"otp_required\",\"username\":\"%s\"}", username);
        http_server_respond_json(msg, 202, resp);
        g_free(resp);
        break;
    }
    default:
        activity_log_event(username, "LOGIN_FAIL", "Bad credentials", ip);
        http_server_respond_error(msg, 401, "Invalid credentials");
        break;
    }

    g_object_unref(parser);
    soup_buffer_free(body);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * [v1 new] 2FA route handlers
 * ═════════════════════════════════════════════════════════════════════════ */

/* POST /api/v1/auth/verify-otp   Body: {username, otp} */
static void _handle_verify_otp(SoupServer *server, SoupMessage *msg, const char *path,
                                GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)query; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    SoupBuffer *body; JsonParser *parser;
    JsonObject *obj = _parse_body(msg, &body, &parser);
    if (!obj) return;

    const char *username = json_object_get_string_member_with_default(obj, "username", "");
    const char *otp      = json_object_get_string_member_with_default(obj, "otp",      "");

    char token_buf[AUTH_TOKEN_BUF] = "";
    AuthLoginResult result = token_auth_verify_otp(srv->auth, username, otp,
                                                   token_buf, sizeof(token_buf));
    switch (result) {
    case AUTH_OK: {
        char *resp = g_strdup_printf("{\"token\":\"%s\"}", token_buf);
        http_server_respond_json(msg, 200, resp);
        g_free(resp);
        break;
    }
    case AUTH_TOTP_REQUIRED:
        http_server_respond_json(msg, 202,
            "{\"status\":\"totp_required\"}");
        break;
    default:
        http_server_respond_error(msg, 401, "Invalid or expired OTP");
        break;
    }

    g_object_unref(parser);
    soup_buffer_free(body);
}

/* POST /api/v1/auth/verify-totp   Body: {username, totp_code} */
static void _handle_verify_totp(SoupServer *server, SoupMessage *msg, const char *path,
                                 GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)query; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    SoupBuffer *body; JsonParser *parser;
    JsonObject *obj = _parse_body(msg, &body, &parser);
    if (!obj) return;

    const char *username   = json_object_get_string_member_with_default(obj, "username",   "");
    const char *totp_code  = json_object_get_string_member_with_default(obj, "totp_code",  "");

    char token_buf[AUTH_TOKEN_BUF] = "";
    AuthLoginResult result = token_auth_verify_totp(srv->auth, username, totp_code,
                                                    token_buf, sizeof(token_buf));
    if (result == AUTH_OK) {
        char *resp = g_strdup_printf("{\"token\":\"%s\"}", token_buf);
        http_server_respond_json(msg, 200, resp);
        g_free(resp);
    } else {
        http_server_respond_error(msg, 401, "Invalid TOTP code");
    }

    g_object_unref(parser);
    soup_buffer_free(body);
}

/* POST /api/v1/auth/setup-totp   (requires valid session) */
static void _handle_setup_totp(SoupServer *server, SoupMessage *msg, const char *path,
                                GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    char user[64], role[16];
    if (!http_server_authorize_ex(srv, msg, query, "viewer",
                                  user, sizeof(user), role, sizeof(role)))
        return;

    char uri[512] = "";
    if (!token_auth_setup_totp(srv->auth, user, _client_ip(client),
                               uri, sizeof(uri))) {
        http_server_respond_error(msg, 500, "TOTP setup failed");
        return;
    }

    char *resp = g_strdup_printf("{\"status\":\"ok\",\"uri\":\"%s\"}", uri);
    http_server_respond_json(msg, 200, resp);
    g_free(resp);
}

/* POST /api/v1/auth/logout   (requires valid session) */
static void _handle_logout(SoupServer *server, SoupMessage *msg, const char *path,
                            GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    const char *token = _extract_token(msg, query);
    char user[64] = "", role[16] = "";
    if (!token || !token_auth_validate(srv->auth, token,
                                       user, sizeof(user), role, sizeof(role))) {
        http_server_respond_error(msg, 401, "Unauthorized"); return;
    }

    token_auth_revoke(srv->auth, token);
    activity_log_event(user, "LOGOUT", "Token revoked", NULL);
    http_server_respond_json(msg, 200, "{\"status\":\"logged_out\"}");
}

/* POST /api/v1/auth/change-password   Body: {old_password, new_password} */
static void _handle_change_password(SoupServer *server, SoupMessage *msg, const char *path,
                                     GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    char user[64], role[16];
    if (!http_server_authorize_ex(srv, msg, query, "viewer",
                                  user, sizeof(user), role, sizeof(role)))
        return;

    SoupBuffer *body; JsonParser *parser;
    JsonObject *obj = _parse_body(msg, &body, &parser);
    if (!obj) return;

    const char *old_pw = json_object_get_string_member_with_default(obj, "old_password", "");
    const char *new_pw = json_object_get_string_member_with_default(obj, "new_password", "");

    char errbuf[1024] = "[]";
    if (!token_auth_change_password(srv->auth, user, old_pw, new_pw,
                                    _client_ip(client), errbuf, sizeof(errbuf))) {
        char *resp = g_strdup_printf(
            "{\"status\":\"error\",\"errors\":%s}", errbuf);
        http_server_respond_json(msg, 400, resp);
        g_free(resp);
    } else {
        http_server_respond_json(msg, 200, "{\"status\":\"ok\"}");
    }

    g_object_unref(parser);
    soup_buffer_free(body);
}

/* POST /api/v1/auth/forgot-password   Body: {username} */
static void _handle_forgot_password(SoupServer *server, SoupMessage *msg, const char *path,
                                     GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)query;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    SoupBuffer *body; JsonParser *parser;
    JsonObject *obj = _parse_body(msg, &body, &parser);
    if (!obj) return;

    const char *username = json_object_get_string_member_with_default(obj, "username", "");
    token_auth_forgot_password(srv->auth, username, _client_ip(client));

    /* Always return success to avoid username enumeration */
    http_server_respond_json(msg, 200,
        "{\"status\":\"ok\",\"message\":\"If the account exists, a reset OTP has been sent.\"}");

    g_object_unref(parser);
    soup_buffer_free(body);
}

/* POST /api/v1/auth/reset-password   Body: {username, otp, new_password} */
static void _handle_reset_password(SoupServer *server, SoupMessage *msg, const char *path,
                                    GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)path; (void)query; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "POST") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    SoupBuffer *body; JsonParser *parser;
    JsonObject *obj = _parse_body(msg, &body, &parser);
    if (!obj) return;

    const char *username = json_object_get_string_member_with_default(obj, "username",     "");
    const char *otp      = json_object_get_string_member_with_default(obj, "otp",          "");
    const char *new_pw   = json_object_get_string_member_with_default(obj, "new_password", "");

    char errbuf[1024] = "[]";
    AuthLoginResult res = token_auth_reset_password(srv->auth, username, otp, new_pw,
                                                    errbuf, sizeof(errbuf));
    if (res == AUTH_OK) {
        http_server_respond_json(msg, 200, "{\"status\":\"ok\"}");
    } else {
        char *resp = g_strdup_printf(
            "{\"status\":\"error\",\"errors\":%s}", errbuf);
        http_server_respond_json(msg, 400, resp);
        g_free(resp);
    }

    g_object_unref(parser);
    soup_buffer_free(body);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * [v1 new] User management  GET/POST/DELETE /api/v1/users
 *
 * GET    /api/v1/users           — list all users (admin)
 * POST   /api/v1/users           — create user   (admin)
 *        Body: {username, password, role, email}
 * DELETE /api/v1/users/<username>— delete user   (admin)
 * ═════════════════════════════════════════════════════════════════════════ */
static void _handle_users(SoupServer *server, SoupMessage *msg, const char *path,
                           GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server;
    HTTPServer *srv = (HTTPServer *)user_data;
    const char *ip  = _client_ip(client);

    char admin_user[64], admin_role[16];
    if (!http_server_authorize_ex(srv, msg, query, "admin",
                                  admin_user, sizeof(admin_user),
                                  admin_role, sizeof(admin_role)))
        return;

    if (g_strcmp0(msg->method, "GET") == 0) {
        /* ── List users ── */
        /* We re-open SQLite directly here since db_manager doesn't expose
         * a list API; in a production system you'd add list_users() there. */
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "users");
        json_builder_begin_array(b);

        /* NOTE: Add list_users() to db_manager for a full implementation.
         * For now we enumerate known roles via a select — replace with
         * db_manager list_users() when available.                          */
        /* Placeholder: return empty array until list_users() is wired.     */

        json_builder_end_array(b);
        json_builder_end_object(b);
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, json_builder_get_root(b));
        char *resp = json_generator_to_data(gen, NULL);
        http_server_respond_json(msg, 200, resp);
        g_free(resp);
        g_object_unref(gen);
        g_object_unref(b);

    } else if (g_strcmp0(msg->method, "POST") == 0) {
        /* ── Create user ── */
        SoupBuffer *body; JsonParser *parser;
        JsonObject *obj = _parse_body(msg, &body, &parser);
        if (!obj) return;

        const char *username = json_object_get_string_member_with_default(obj, "username", "");
        const char *password = json_object_get_string_member_with_default(obj, "password", "");
        const char *role     = json_object_get_string_member_with_default(obj, "role",     "viewer");
        const char *email    = json_object_get_string_member_with_default(obj, "email",    "");

        if (!*username || !*password) {
            http_server_respond_error(msg, 400, "username and password are required");
        } else if (user_exists(username)) {
            http_server_respond_error(msg, 409, "User already exists");
        } else {
            token_auth_add_user(srv->auth, username, password, role, email);
            char detail[128];
            snprintf(detail, sizeof(detail), "Created by admin '%s'", admin_user);
            activity_log_event(username, "USER_CREATED", detail, ip);
            http_server_respond_json(msg, 201, "{\"status\":\"created\"}");
        }

        g_object_unref(parser);
        soup_buffer_free(body);

    } else if (g_strcmp0(msg->method, "DELETE") == 0) {
        /* ── Delete user  DELETE /api/v1/users/<username> ── */
        const char *slash = strrchr(path, '/');
        const char *target = (slash && strlen(slash) > 1) ? slash + 1 : NULL;

        if (!target) {
            http_server_respond_error(msg, 400, "Username required in path");
            return;
        }
        if (!token_auth_delete_user(srv->auth, target, admin_user, ip)) {
            http_server_respond_error(msg, 404, "User not found");
        } else {
            http_server_respond_json(msg, 200, "{\"status\":\"deleted\"}");
        }
    } else {
        http_server_respond_error(msg, 405, "Method not allowed");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * [v1 new] Device controls  GET/POST /api/v1/device/<action>
 *
 * GET  /api/v1/device/status        — device status    (viewer)
 * POST /api/v1/device/reboot        — reboot           (admin)
 * POST /api/v1/device/network-reset — restart network  (operator)
 * POST /api/v1/device/config-reset  — restore config   (admin)
 * POST /api/v1/device/factory-reset — wipe everything  (admin, body: {confirm:true})
 * ═════════════════════════════════════════════════════════════════════════ */
static void _handle_device(SoupServer *server, SoupMessage *msg, const char *path,
                            GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server;
    HTTPServer *srv   = (HTTPServer *)user_data;
    const char *ip    = _client_ip(client);

    /* Determine action from path suffix */
    const char *action = strrchr(path, '/');
    if (!action || strlen(action) <= 1) {
        http_server_respond_error(msg, 400, "Device action required in path");
        return;
    }
    action++; /* skip leading '/' */

    /* ── Per-action role requirements ── */
    const char *required_role = "admin"; /* default: admin */
    if (g_strcmp0(action, "status") == 0)        required_role = "viewer";
    else if (g_strcmp0(action, "network-reset") == 0) required_role = "operator";

    char user[64], role[16];
    if (!http_server_authorize_ex(srv, msg, query, required_role,
                                  user, sizeof(user), role, sizeof(role)))
        return;

    /* Build v1 Session for device_controls */
    Session sess;
    _make_session(user, role, &sess);

    char result_buf[4096];
    int rc = 0;

    if (g_strcmp0(action, "status") == 0) {
        if (g_strcmp0(msg->method, "GET") != 0) goto bad_method;
        rc = device_status(&sess, ip, result_buf, sizeof(result_buf));

    } else if (g_strcmp0(action, "reboot") == 0) {
        if (g_strcmp0(msg->method, "POST") != 0) goto bad_method;
        rc = device_reboot(&sess, ip, result_buf, sizeof(result_buf));

    } else if (g_strcmp0(action, "network-reset") == 0) {
        if (g_strcmp0(msg->method, "POST") != 0) goto bad_method;
        rc = network_reset(&sess, ip, result_buf, sizeof(result_buf));

    } else if (g_strcmp0(action, "config-reset") == 0) {
        if (g_strcmp0(msg->method, "POST") != 0) goto bad_method;
        rc = config_reset(&sess, ip, result_buf, sizeof(result_buf));

    } else if (g_strcmp0(action, "factory-reset") == 0) {
        if (g_strcmp0(msg->method, "POST") != 0) goto bad_method;

        /* Require explicit confirm flag in body */
        SoupBuffer *body; JsonParser *parser;
        JsonObject *obj = _parse_body(msg, &body, &parser);
        if (!obj) return;
        gboolean confirm = json_object_get_boolean_member_with_default(
                               obj, "confirm", FALSE);
        g_object_unref(parser);
        soup_buffer_free(body);

        rc = factory_reset(&sess, ip, (int)confirm,
                           result_buf, sizeof(result_buf));
        if (rc == -2) {
            http_server_respond_error(msg, 400,
                "confirm:true required for factory reset");
            return;
        }
    } else {
        http_server_respond_error(msg, 404, "Unknown device action");
        return;
    }

    if (rc == 0) {
        http_server_respond_json(msg, 200, result_buf);
    } else {
        http_server_respond_error(msg, 500, "Device operation failed");
    }
    return;

bad_method:
    http_server_respond_error(msg, 405, "Method not allowed for this action");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * [v2 original] HLS streaming, static recordings, AI, system, lenses
 * ═════════════════════════════════════════════════════════════════════════ */

static void _handle_hls_stream(SoupServer *server, SoupMessage *msg, const char *path,
                                GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;
    if (!http_server_authorize(srv, msg, query, "viewer")) return;

    soup_message_headers_append(msg->response_headers, "Access-Control-Allow-Origin", "*");
    soup_message_headers_append(msg->response_headers, "Access-Control-Allow-Methods", "GET, OPTIONS");
    soup_message_headers_append(msg->response_headers, "Cache-Control", "no-cache, no-store, must-revalidate");

    if (g_strcmp0(msg->method, "OPTIONS") == 0) {
        soup_message_set_status(msg, 204); return;
    }

    char **parts = g_strsplit(path, "/", -1);
    int count = g_strv_length(parts);
    if (count < 7) {
        http_server_respond_error(msg, 404, "Invalid stream path");
        g_strfreev(parts); return;
    }

    const char *lens     = parts[4];
    const char *tier     = parts[5];
    const char *filename = parts[6];

    if (g_strcmp0(filename, "playlist.m3u8") == 0) {
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (bus) {
            char *json = g_strdup_printf(
                "{\"lens\":\"%s\",\"branch\":\"%s\","
                "\"output_dir\":\"/data/hls/%s/%s\"}",
                lens, tier, lens, tier);
            g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, "StartHLS", g_variant_new("(s)", json),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            g_free(json);
            g_object_unref(bus);
        }
    }

    char *local_path = g_strdup_printf("/data/hls/%s/%s/%s", lens, tier, filename);
    gchar *contents = NULL; gsize len;
    int retries = 0;
    while (retries < 30) {
        if (g_file_get_contents(local_path, &contents, &len, NULL)) break;
        if (g_strcmp0(filename, "playlist.m3u8") == 0) { g_usleep(100000); retries++; }
        else break;
    }

    if (contents) {
        const char *mime = "application/octet-stream";
        if (g_str_has_suffix(filename, ".m3u8")) mime = "application/vnd.apple.mpegurl";
        else if (g_str_has_suffix(filename, ".ts")) mime = "video/MP2T";
        soup_message_set_response(msg, mime, SOUP_MEMORY_TAKE, contents, len);
        soup_message_set_status(msg, 200);
    } else {
        http_server_respond_error(msg, 404, "Stream resource not found");
    }

    g_free(local_path);
    g_strfreev(parts);
}

void _handle_static_recordings(SoupServer *server, SoupMessage *msg, const char *path,
                                GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;
    if (!http_server_authorize(srv, msg, query, "viewer")) return;

    if (g_strcmp0(msg->method, "GET") != 0) {
        http_server_respond_error(msg, 405, "Method not allowed"); return;
    }

    const char *filename = strrchr(path, '/');
    if (!filename || strlen(filename) <= 1) {
        http_server_respond_error(msg, 404, "File not found"); return;
    }
    filename++;

    char *full_path = g_build_filename("/data", filename, NULL);
    gchar *contents = NULL; gsize len;
    if (g_file_get_contents(full_path, &contents, &len, NULL)) {
        soup_message_set_response(msg, "video/mp4", SOUP_MEMORY_TAKE, contents, len);
        soup_message_set_status(msg, 200);
    } else {
        http_server_respond_error(msg, 404, "Recording not found");
    }
    g_free(full_path);
}

/* ── System handler (overridden by main.c's _route_system, kept for parity) */
static void _handle_system(SoupServer *server, SoupMessage *msg, const char *path,
                            GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) { http_server_respond_error(msg, 500, "Bus error"); return; }
    char *result = NULL;

    if (g_str_has_suffix(path, "/status") && g_strcmp0(msg->method, "GET") == 0) {
        if (!http_server_authorize(srv, msg, query, "viewer")) { g_object_unref(bus); return; }
        GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
            UNIFIED_BUS_NAME, "GetStatus", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
        if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
    } else if (g_str_has_suffix(path, "/log_level") && g_strcmp0(msg->method, "PUT") == 0) {
        if (!http_server_authorize(srv, msg, query, "admin")) { g_object_unref(bus); return; }
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, msg->request_body->data, msg->request_body->length, NULL)) {
            const char *lvl = json_object_get_string_member_with_default(
                json_node_get_object(json_parser_get_root(p)), "level", "INFO");
            GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, "SetLogLevel", g_variant_new("(s)", lvl),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
        }
        g_object_unref(p);
    }

    if (result) { http_server_respond_json(msg, 200, result); g_free(result); }
    else        { http_server_respond_error(msg, 404, "Not found"); }
    g_object_unref(bus);
}

/* ── Lenses handler (overridden by main.c's _route_lenses, kept for parity) */
static void _handle_lenses(SoupServer *server, SoupMessage *msg, const char *path,
                            GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;
    if (!http_server_authorize(srv, msg, query, "operator")) return;

    char **parts = g_strsplit(path, "/", -1);
    int count = g_strv_length(parts);
    if (count < 5) { http_server_respond_error(msg, 400, "Invalid path"); g_strfreev(parts); return; }

    const char *lens = parts[4];
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    char *result = NULL;

    if (bus) {
        if (count >= 6 && g_strcmp0(parts[5], "recording") == 0) {
            const char *m = (g_strcmp0(msg->method, "POST") == 0) ? "StartRecording" : "StopRecording";
            char *json = (msg->request_body && msg->request_body->length > 0)
                ? g_strndup(msg->request_body->data, msg->request_body->length)
                : g_strdup_printf("{\"lens\":\"%s\",\"branch\":\"main\",\"output_dir\":\"/data\"}", lens);
            GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, m, g_variant_new("(s)", json), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
            g_free(json);
        } else if (count >= 6 && g_strcmp0(parts[5], "snapshot") == 0) {
            char ts[64]; snprintf(ts, sizeof(ts), "/data/snap_%s_%ld.jpg", lens, time(NULL));
            GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, "TakeSnapshot", g_variant_new("(ss)", lens, ts),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
        } else if (count >= 7 && g_strcmp0(parts[5], "params") == 0) {
            char *json = g_strndup(msg->request_body->data, msg->request_body->length);
            GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, "UpdateStreamParams", g_variant_new("(sss)", lens, parts[6], json),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
            g_free(json);
        } else if (count >= 6 && g_strcmp0(parts[5], "config") == 0) {
            char *json = g_strndup(msg->request_body->data, msg->request_body->length);
            GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, "ConfigureLens", g_variant_new("(s)", json),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
            g_free(json);
        } else if (count >= 6 && g_strcmp0(parts[5], "overlay") == 0) {
            char *json = g_strndup(msg->request_body->data, msg->request_body->length);
            GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, "ToggleOverlay", g_variant_new("(s)", json),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
            g_free(json);
        } else if (count >= 6 && g_strcmp0(parts[5], "ntp_overlay") == 0) {
            char *json = g_strndup(msg->request_body->data, msg->request_body->length);
            GVariant *v = g_dbus_connection_call_sync(bus, UNIFIED_BUS_NAME, UNIFIED_BUS_PATH,
                UNIFIED_BUS_NAME, "ToggleNTP", g_variant_new("(s)", json),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
            g_free(json);
        }
        g_object_unref(bus);
    }

    if (result) { http_server_respond_json(msg, 200, result); g_free(result); }
    else        { http_server_respond_error(msg, 404, "Not found or error"); }
    g_strfreev(parts);
}

/* ── Recordings API (overridden by main.c's _route_recordings) */
static void _handle_recordings_api(SoupServer *server, SoupMessage *msg, const char *path,
                                    GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)query; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;

    if (g_strcmp0(msg->method, "GET") == 0) {
        if (!http_server_authorize(srv, msg, query, "viewer")) return;
        GDir *dir = g_dir_open("/data", 0, NULL);
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "recordings");
        json_builder_begin_array(b);
        if (dir) {
            const char *f;
            while ((f = g_dir_read_name(dir)))
                if (g_str_has_suffix(f, ".mp4")) json_builder_add_string_value(b, f);
            g_dir_close(dir);
        }
        json_builder_end_array(b);
        json_builder_end_object(b);
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, json_builder_get_root(b));
        char *resp = json_generator_to_data(gen, NULL);
        http_server_respond_json(msg, 200, resp);
        g_free(resp); g_object_unref(gen); g_object_unref(b);
    } else if (g_strcmp0(msg->method, "DELETE") == 0) {
        if (!http_server_authorize(srv, msg, query, "operator")) return;
        char **parts = g_strsplit(path, "/", -1);
        if (g_strv_length(parts) >= 5) {
            char *full = g_build_filename("/data", parts[4], NULL);
            if (g_remove(full) == 0) http_server_respond_json(msg, 200, "{\"status\":\"deleted\"}");
            else http_server_respond_error(msg, 404, "File not found");
            g_free(full);
        }
        g_strfreev(parts);
    } else {
        http_server_respond_error(msg, 405, "Method not allowed");
    }
}

/* ── AI handler (unchanged from v2) */
static void _handle_ai(SoupServer *server, SoupMessage *msg, const char *path,
                        GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    HTTPServer *srv = (HTTPServer *)user_data;
    if (!http_server_authorize(srv, msg, query, "operator")) return;

    GError *dbus_err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &dbus_err);
    if (!bus) {
        http_server_respond_error(msg, 500, "D-Bus unavailable"); return;
    }
    char *result = NULL;
    gboolean matched = FALSE;

    if (g_str_has_suffix(path, "/status") && g_strcmp0(msg->method, "GET") == 0) {
        matched = TRUE;
        GVariant *v = g_dbus_connection_call_sync(bus, "com.camera.AIEngine",
            "/com/camera/AIEngine", "com.camera.AIEngine", "GetStatus",
            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &dbus_err);
        if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
    } else if (g_str_has_suffix(path, "/load") && g_strcmp0(msg->method, "POST") == 0) {
        matched = TRUE;
        SoupBuffer *body = soup_message_body_flatten(msg->request_body);
        if (body && body->length > 0) {
            GVariant *v = g_dbus_connection_call_sync(bus, "com.camera.AIEngine",
                "/com/camera/AIEngine", "com.camera.AIEngine", "LoadModel",
                g_variant_new("(s)", body->data), NULL, G_DBUS_CALL_FLAGS_NONE, 15000, NULL, &dbus_err);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
        }
        if (body) soup_buffer_free(body);
    } else if (g_str_has_suffix(path, "/stop") && g_strcmp0(msg->method, "POST") == 0) {
        matched = TRUE;
        SoupBuffer *body = soup_message_body_flatten(msg->request_body);
        if (body && body->length > 0) {
            GVariant *v = g_dbus_connection_call_sync(bus, "com.camera.AIEngine",
                "/com/camera/AIEngine", "com.camera.AIEngine", "StopModel",
                g_variant_new("(s)", body->data), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &dbus_err);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
        }
        if (body) soup_buffer_free(body);
    } else if (g_str_has_suffix(path, "/config") && g_strcmp0(msg->method, "POST") == 0) {
        matched = TRUE;
        SoupBuffer *body = soup_message_body_flatten(msg->request_body);
        if (body && body->length > 0) {
            GVariant *v = g_dbus_connection_call_sync(bus, "com.camera.AIEngine",
                "/com/camera/AIEngine", "com.camera.AIEngine", "SetAIConfig",
                g_variant_new("(s)", body->data), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &dbus_err);
            if (v) { g_variant_get(v, "(s)", &result); g_variant_unref(v); }
        }
        if (body) soup_buffer_free(body);
    }

    if (result) {
        http_server_respond_json(msg, 200, result); g_free(result);
    } else if (!matched) {
        http_server_respond_error(msg, 404, "Unknown AI endpoint");
    } else if (dbus_err) {
        char *e = g_strdup_printf("{\"error\":\"AI Engine unreachable: %s\"}", dbus_err->message);
        http_server_respond_json(msg, 503, e); g_free(e);
    } else {
        http_server_respond_error(msg, 500, "AI Engine returned no response");
    }

    if (dbus_err) g_error_free(dbus_err);
    g_object_unref(bus);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═════════════════════════════════════════════════════════════════════════ */

HTTPServer *http_server_new(TokenAuth *auth, int port, gboolean enable_tls)
{
    HTTPServer *srv = g_new0(HTTPServer, 1);
    srv->auth = auth;
    srv->port = port;
    srv->tls  = enable_tls;
    srv->soup = soup_server_new(SOUP_SERVER_SERVER_HEADER, "SmartIP-Edge", NULL);

    /* ── v2 original routes ── */
    soup_server_add_handler(srv->soup, "/",                       _handle_root,             srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/auth/login",      _handle_login,            srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/health",          _handle_health,           srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/stream",          _handle_hls_stream,       srv, NULL);
    soup_server_add_handler(srv->soup, "/recordings",             _handle_static_recordings,srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/system",          _handle_system,           srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/lenses",          _handle_lenses,           srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/recordings",      _handle_recordings_api,   srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/ai",              _handle_ai,               srv, NULL);

    /* ── v1 new: 2FA auth routes ── */
    soup_server_add_handler(srv->soup, "/api/v1/auth/verify-otp",      _handle_verify_otp,      srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/auth/verify-totp",     _handle_verify_totp,     srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/auth/setup-totp",      _handle_setup_totp,      srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/auth/logout",          _handle_logout,          srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/auth/change-password", _handle_change_password, srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/auth/forgot-password", _handle_forgot_password, srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/auth/reset-password",  _handle_reset_password,  srv, NULL);

    /* ── v1 new: user management & device controls ── */
    soup_server_add_handler(srv->soup, "/api/v1/users",   _handle_users,  srv, NULL);
    soup_server_add_handler(srv->soup, "/api/v1/device",  _handle_device, srv, NULL);

    return srv;
}

SoupServer *http_server_get_soup(HTTPServer *srv) { return srv->soup; }

void http_server_start(HTTPServer *srv)
{
    GError *err = NULL;

    if (srv->tls) {
        const char *cert_dir  = "/tmp/demo_certs";
        char cert_path[256], key_path[256];
        snprintf(cert_path, sizeof(cert_path), "%s/demo_cert.pem", cert_dir);
        snprintf(key_path,  sizeof(key_path),  "%s/demo_key.pem",  cert_dir);

        if (!g_file_test(cert_path, G_FILE_TEST_EXISTS)) {
            g_mkdir_with_parents(cert_dir, 0755);
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
                "-days 365 -nodes -subj '/CN=demo-camera/O=IPCameraDemo/C=IN' "
                "2>/dev/null", key_path, cert_path);
            if (system(cmd) != 0) {
                g_warning("Failed to generate TLS cert — falling back to HTTP");
                srv->tls = FALSE;
            } else {
                g_info("Self-signed TLS cert generated: %s", cert_path);
            }
        }

        if (srv->tls) {
            GTlsCertificate *cert = g_tls_certificate_new_from_files(
                cert_path, key_path, &err);
            if (cert) {
                soup_server_set_ssl_cert_file(srv->soup, cert_path, key_path, &err);
                if (err) {
                    g_warning("TLS setup failed: %s — falling back to HTTP", err->message);
                    g_error_free(err); err = NULL; srv->tls = FALSE;
                }
                g_object_unref(cert);
            } else {
                g_warning("Cert load failed: %s — falling back to HTTP", err->message);
                g_error_free(err); err = NULL; srv->tls = FALSE;
            }
        }
    }

    SoupServerListenOptions opts = srv->tls ? SOUP_SERVER_LISTEN_HTTPS : 0;
    if (!soup_server_listen_all(srv->soup, srv->port, opts, &err)) {
        g_warning("HTTP server failed on port %d: %s", srv->port, err->message);
        g_error_free(err);
        return;
    }
    g_info("%s server listening on port %d",
           srv->tls ? "HTTPS" : "HTTP", srv->port);
}

void http_server_stop(HTTPServer *srv)
{
    if (srv) soup_server_disconnect(srv->soup);
}

void http_server_free(HTTPServer *srv)
{
    if (!srv) return;
    http_server_stop(srv);
    g_object_unref(srv->soup);
    g_free(srv);
}
