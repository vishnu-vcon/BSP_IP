/*
 * main.c — Control Plane Entry Point (Binary 2) — Merged v1+v2
 * ==============================================================
 *
 * v1 additions vs original main.c:
 *   - Passes smtp_conf_path to token_auth_new()
 *   - Adds email addresses when seeding default users
 *   - Logs SERVER_START / SERVER_STOP via activity_logger
 *   - /api/v1/device and /api/v1/users routes are registered inside
 *     http_server_new() (no explicit soup_server_add_handler calls needed here)
 *   - The main.c-registered routes (_route_lenses / _route_system /
 *     _route_recordings) still override the internal http_server.c versions
 *     exactly as before, providing the cached cp->engine_proxy D-Bus path.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../common/config.h"
#include "../common/event_broker.h"
#include "core/auth.h"
#include "core/http_server.h"

/* v1: activity logger for SERVER_START / SERVER_STOP events */
#include "../auth/activity_logger.h"

/* ─────────────────────────────────────────────────────────────────────────
 * Control plane state
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct {
    GMainLoop    *loop;
    HTTPServer   *http;
    TokenAuth    *auth;
    GDBusProxy   *engine_proxy;
    MQSubscriber *subscriber;
} ControlPlane;

static ControlPlane    *g_cp = NULL;
static volatile gint    g_app_log_level = G_LOG_LEVEL_INFO;

/* ─────────────────────────────────────────────────────────────────────────
 * D-Bus call helper
 * ───────────────────────────────────────────────────────────────────────── */

static char *_call_engine(ControlPlane *cp, const char *method, GVariant *params)
{
    if (!cp->engine_proxy)
        return g_strdup("{\"status\":\"error\",\"message\":\"Engine not connected\"}");

    GError *err = NULL;
    GVariant *result = g_dbus_proxy_call_sync(cp->engine_proxy, method, params,
                                              G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
    if (err) {
        char *msg = g_strdup_printf(
            "{\"status\":\"error\",\"message\":\"D-Bus err: %s\"}", err->message);
        g_error_free(err);
        return msg;
    }
    const gchar *resp;
    g_variant_get(result, "(&s)", &resp);
    char *ret = g_strdup(resp);
    g_variant_unref(result);
    return ret;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Route: /api/v1/lenses/<lens>/<action>
 * (overrides http_server.c's _handle_lenses — uses cached engine_proxy)
 * ───────────────────────────────────────────────────────────────────────── */

static void _route_lenses(SoupServer *server, SoupMessage *msg, const char *path,
                           GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    ControlPlane *cp = (ControlPlane *)user_data;
    if (!http_server_authorize(cp->http, msg, query, "operator")) return;

    gchar **parts    = g_strsplit(path, "/", -1);
    int     num_parts = g_strv_length(parts);

    if (num_parts < 6) {
        http_server_respond_error(msg, 404, "Invalid lens resource path");
        g_strfreev(parts); return;
    }

    const char *lens   = parts[4];
    /* Strip query string from action if present */
    const char *raw    = parts[5];
    const char *q      = strchr(raw, '?');
    char        action_buf[64];
    if (q) { int n = (int)(q - raw); snprintf(action_buf, sizeof(action_buf), "%.*s", n, raw); }
    else     g_strlcpy(action_buf, raw, sizeof(action_buf));
    const char *action = action_buf;

    char *result = NULL;

    if (g_strcmp0(action, "config") == 0) {
        if (g_strcmp0(msg->method, "PATCH") != 0) goto bad_method;
        SoupBuffer *body = soup_message_body_flatten(msg->request_body);
        JsonParser *p    = json_parser_new();
        if (body->length > 0 &&
            json_parser_load_from_data(p, body->data, body->length, NULL)) {
            JsonObject *obj = json_node_get_object(json_parser_get_root(p));
            json_object_set_string_member(obj, "lens", lens);
            JsonGenerator *gen = json_generator_new();
            json_generator_set_root(gen, json_parser_get_root(p));
            char *pj = json_generator_to_data(gen, NULL);
            result = _call_engine(cp, "ConfigureLens", g_variant_new("(s)", pj));
            g_free(pj); g_object_unref(gen);
        } else {
            http_server_respond_error(msg, 400, "Invalid JSON payload");
        }
        g_object_unref(p); soup_buffer_free(body);

    } else if (g_strcmp0(action, "recording") == 0) {
        if (g_strcmp0(msg->method, "POST") == 0) {
            SoupBuffer *body = soup_message_body_flatten(msg->request_body);
            char *json;
            if (body->length > 0) {
                json = g_strndup(body->data, body->length);
            } else {
                json = g_strdup_printf(
                    "{\"lens\":\"%s\",\"branch\":\"main\"}", lens);
            }
            result = _call_engine(cp, "StartRecording", g_variant_new("(s)", json));
            g_free(json); soup_buffer_free(body);
        } else if (g_strcmp0(msg->method, "DELETE") == 0) {
            char params[256];
            snprintf(params, sizeof(params),
                     "{\"lens\":\"%s\",\"branch\":\"main\"}", lens);
            result = _call_engine(cp, "StopRecording", g_variant_new("(s)", params));
        } else goto bad_method;

    } else if (g_strcmp0(action, "snapshot") == 0) {
        if (g_strcmp0(msg->method, "POST") != 0) goto bad_method;
        char path_out[256];
        snprintf(path_out, sizeof(path_out), "/data/snap_%s_%ld.jpg", lens, time(NULL));
        result = _call_engine(cp, "TakeSnapshot", g_variant_new("(ss)", lens, path_out));

    } else if (g_strcmp0(action, "streams") == 0) {
        if (g_strcmp0(msg->method, "PATCH") != 0) goto bad_method;
        if (num_parts < 8 || g_strcmp0(parts[7], "params") != 0) {
            http_server_respond_error(msg, 404,
                "Expected /lenses/<lens>/streams/<branch>/params");
            g_strfreev(parts); return;
        }
        SoupBuffer *body = soup_message_body_flatten(msg->request_body);
        if (body->length > 0) {
            char *body_str = g_strndup(body->data, body->length);
            result = _call_engine(cp, "UpdateStreamParams",
                                  g_variant_new("(sss)", lens, parts[6], body_str));
            g_free(body_str);
        } else {
            http_server_respond_error(msg, 400, "JSON body required");
        }
        soup_buffer_free(body);

    } else if (g_strcmp0(action, "overlay") == 0) {
        if (g_strcmp0(msg->method, "PATCH") != 0) goto bad_method;
        SoupBuffer *body = soup_message_body_flatten(msg->request_body);
        if (body->length > 0) {
            char *body_str = g_strndup(body->data, body->length);
            result = _call_engine(cp, "ToggleOverlay", g_variant_new("(s)", body_str));
            g_free(body_str);
        } else {
            http_server_respond_error(msg, 400, "JSON body required");
        }
        soup_buffer_free(body);

    } else {
        http_server_respond_error(msg, 404, "Unknown lens action");
        g_strfreev(parts); return;
    }

    if (result) {
        http_server_respond_json(msg, 200, result); g_free(result);
    } else if (msg->status_code == SOUP_STATUS_OK) {
        http_server_respond_error(msg, 500, "Internal engine call failed");
    }
    g_strfreev(parts); return;

bad_method:
    http_server_respond_error(msg, 405, "Method not allowed for this resource");
    g_strfreev(parts);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Route: /api/v1/system/<param>
 * ───────────────────────────────────────────────────────────────────────── */

static void _route_system(SoupServer *server, SoupMessage *msg, const char *path,
                           GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    ControlPlane *cp = (ControlPlane *)user_data;

    if (g_str_has_suffix(path, "/status")) {
        if (!http_server_authorize(cp->http, msg, query, "viewer")) return;
        if (g_strcmp0(msg->method, "GET") != 0) goto bad_method;
        char *result = _call_engine(cp, "GetStatus", NULL);
        if (result) { http_server_respond_json(msg, 200, result); g_free(result); }
        else          http_server_respond_error(msg, 500, "Failed to connect to engine");

    } else if (g_str_has_suffix(path, "/log_level")) {
        if (!http_server_authorize(cp->http, msg, query, "admin")) return;
        if (g_strcmp0(msg->method, "PUT") != 0) goto bad_method;

        SoupBuffer *body = soup_message_body_flatten(msg->request_body);
        JsonParser *p    = json_parser_new();
        if (json_parser_load_from_data(p, body->data, body->length, NULL)) {
            JsonObject *obj = json_node_get_object(json_parser_get_root(p));
            const char *level = json_object_get_string_member_with_default(
                obj, "level", "INFO");

            if      (g_strcmp0(level, "DEBUG")   == 0) g_app_log_level = G_LOG_LEVEL_DEBUG;
            else if (g_strcmp0(level, "INFO")    == 0) g_app_log_level = G_LOG_LEVEL_INFO;
            else if (g_strcmp0(level, "WARNING") == 0) g_app_log_level = G_LOG_LEVEL_WARNING;
            else if (g_strcmp0(level, "ERROR")   == 0) g_app_log_level = G_LOG_LEVEL_ERROR;
            g_info("Control plane log level set to: %s", level);

            char *result = _call_engine(cp, "SetLogLevel", g_variant_new("(s)", level));
            http_server_respond_json(msg, 200, result);
            g_free(result);
        } else {
            http_server_respond_error(msg, 400, "Invalid JSON");
        }
        g_object_unref(p);
        soup_buffer_free(body);
    } else {
        http_server_respond_error(msg, 404, "Unknown system resource");
    }
    return;

bad_method:
    http_server_respond_error(msg, 405, "Method not allowed");
}

/* ─────────────────────────────────────────────────────────────────────────
 * Route: /api/v1/recordings
 * ───────────────────────────────────────────────────────────────────────── */

static void _route_recordings(SoupServer *server, SoupMessage *msg, const char *path,
                               GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)client;
    ControlPlane *cp       = (ControlPlane *)user_data;
    const char   *dir_path = "/data";

    if (g_strcmp0(msg->method, "GET") == 0) {
        if (!http_server_authorize(cp->http, msg, query, "viewer")) return;

        GDir *dir = g_dir_open(dir_path, 0, NULL);
        if (!dir) {
            http_server_respond_json(msg, 200, "{\"recordings\":[]}"); return;
        }
        JsonArray *arr = json_array_new();
        const gchar *filename;
        while ((filename = g_dir_read_name(dir)))
            if (g_str_has_suffix(filename, ".mp4"))
                json_array_add_string_element(arr, filename);
        g_dir_close(dir);

        JsonObject    *root = json_object_new();
        json_object_set_array_member(root, "recordings", arr);
        JsonGenerator *gen  = json_generator_new();
        json_generator_set_root(gen, json_node_init_object(json_node_alloc(), root));
        char *json = json_generator_to_data(gen, NULL);
        http_server_respond_json(msg, 200, json);
        g_free(json); g_object_unref(gen); json_object_unref(root);

    } else if (g_strcmp0(msg->method, "DELETE") == 0) {
        if (!http_server_authorize(cp->http, msg, query, "operator")) return;
        const char *fn = strrchr(path, '/');
        if (fn && strlen(fn) > 1) {
            fn++;
            char *full = g_build_filename(dir_path, fn, NULL);
            if (g_remove(full) == 0)
                http_server_respond_json(msg, 200, "{\"status\":\"deleted\"}");
            else
                http_server_respond_error(msg, 404, "File not found or access denied");
            g_free(full);
        } else {
            http_server_respond_error(msg, 400, "Filename required in path");
        }
    } else {
        http_server_respond_error(msg, 405, "Method not allowed");
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * ZMQ alert callback
 * ───────────────────────────────────────────────────────────────────────── */

static void _on_alert(const char *topic, const char *json, gpointer user_data)
{
    (void)user_data;
    g_info("Alert received [%s]: %s", topic, json);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Signal handler
 * ───────────────────────────────────────────────────────────────────────── */

static void _signal_handler(int signum)
{
    (void)signum;
    if (g_cp && g_cp->loop) g_main_loop_quit(g_cp->loop);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Dynamic log handler
 * ───────────────────────────────────────────────────────────────────────── */

static void _custom_log_handler(const gchar *log_domain, GLogLevelFlags log_level,
                                const gchar *message, gpointer user_data)
{
    (void)user_data;
    if (log_level > g_app_log_level && log_level != G_LOG_LEVEL_MESSAGE) return;
    g_log_default_handler(log_domain, log_level, message, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main()
 * ═════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    gint     http_port = DEFAULT_HTTP_PORT;
    gboolean no_tls    = FALSE;
    /* Optional SMTP config path — override with --smtp-conf */
    gchar   *smtp_conf = NULL;

    GOptionEntry entries[] = {
        { "port",      'p', 0, G_OPTION_ARG_INT,      &http_port, "HTTP port",           "PORT" },
        { "no-tls",     0,  0, G_OPTION_ARG_NONE,     &no_tls,    "Disable TLS",         NULL   },
        { "smtp-conf",  0,  0, G_OPTION_ARG_FILENAME, &smtp_conf, "SMTP config file",    "PATH" },
        { NULL }
    };

    GOptionContext *ctx = g_option_context_new("- SMART IP Edge Control Plane");
    g_option_context_add_main_entries(ctx, entries, NULL);
    GError *err = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
        g_printerr("Option error: %s\n", err->message); return 1;
    }
    g_option_context_free(ctx);

    g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
    g_log_set_default_handler(_custom_log_handler, NULL);

    g_info("═══ SMART IP Edge Control Plane (C) ═══");

    ControlPlane *cp = g_new0(ControlPlane, 1);
    cp->loop = g_main_loop_new(NULL, FALSE);

    /* Persistent storage */
    g_mkdir_with_parents("/data", 0755);

    /* ── Auth setup (v1: init_db() called inside token_auth_new) ── */
    cp->auth = token_auth_new(NULL, 7200, smtp_conf);

    /*
     * Seed default users.  Passwords will be bcrypt-hashed and stored in
     * SQLite.  These are no-ops if the rows already exist (INSERT OR IGNORE).
     * Production deployments should remove or replace these seeds.
     *
     * Email addresses are required for the 2FA OTP flow; leave empty to
     * skip 2FA for a user (useful for embedded/headless test accounts).
     */
    token_auth_add_user(cp->auth, "admin",    "admin123",    "admin",    "");
    token_auth_add_user(cp->auth, "operator", "operator123", "operator", "");
    token_auth_add_user(cp->auth, "viewer",   "viewer123",   "viewer",   "");

    /* ── D-Bus connection to Unified Engine ── */
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (bus) {
        cp->engine_proxy = g_dbus_proxy_new_sync(bus,
            G_DBUS_PROXY_FLAGS_NONE, NULL,
            UNIFIED_BUS_NAME, UNIFIED_BUS_PATH, UNIFIED_BUS_NAME,
            NULL, &err);
        if (cp->engine_proxy) {
            g_info("Connected to Unified Engine via D-Bus");
        } else {
            g_warning("Could not create D-Bus proxy: %s",
                      err ? err->message : "unknown");
            if (err) { g_error_free(err); err = NULL; }
        }
    }

    /* ── HTTP server (registers built-in + all new v1 routes) ── */
    cp->http = http_server_new(cp->auth, http_port, !no_tls);

    /*
     * Override the internal http_server.c handlers for lenses / system /
     * recordings with main.c versions that use the cached cp->engine_proxy.
     * This is the same pattern as the original v2 code.
     */
    SoupServer *soup = http_server_get_soup(cp->http);
    soup_server_add_handler(soup, "/api/v1/lenses",     _route_lenses,     cp, NULL);
    soup_server_add_handler(soup, "/api/v1/system",     _route_system,     cp, NULL);
    soup_server_add_handler(soup, "/api/v1/recordings", _route_recordings, cp, NULL);

    http_server_start(cp->http);

    /* ── ZMQ subscriber ── */
    cp->subscriber = mq_subscriber_new(ZMQ_DEFAULT_PORT);
    mq_subscriber_subscribe(cp->subscriber, TOPIC_ALERTS);
    mq_subscriber_set_callback(cp->subscriber, _on_alert, cp);
    mq_subscriber_start_background(cp->subscriber);

    /* v1: log server start event */
    activity_log_event("system", "SERVER_START",
                       "Control Plane started", NULL);

    g_cp = cp;
    signal(SIGINT,  _signal_handler);
    signal(SIGTERM, _signal_handler);

    g_main_loop_run(cp->loop);

    /* ── Cleanup ── */
    activity_log_event("system", "SERVER_STOP",
                       "Control Plane shutting down", NULL);

    mq_subscriber_free(cp->subscriber);
    http_server_free(cp->http);
    token_auth_free(cp->auth);
    if (cp->engine_proxy) g_object_unref(cp->engine_proxy);
    g_main_loop_unref(cp->loop);
    g_free(cp);
    g_free(smtp_conf);

    g_info("Control Plane shut down.");
    return 0;
}
