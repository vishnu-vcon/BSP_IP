/*
 * main.c — Demo CLI (Binary 4)
 * ===============================
 * Process 4: Interactive CLI that acts as a web mock client 
 *            communicating via HTTP/REST to the Control Plane.
 */

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#include "../common/config.h"

#define BASE_URL "https://127.0.0.1:8443/api/v1"

static SoupSession *session = NULL;
static char *auth_token = NULL;

static char *call_api(const char *method, const char *path, const char *json_body)
{
    char url[512];
    snprintf(url, sizeof(url), "%s%s", BASE_URL, path);

    SoupMessage *msg = soup_message_new(method, url);
    if (!msg) return g_strdup("Failed to create message");

    if (auth_token) {
        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", auth_token);
        soup_message_headers_append(msg->request_headers, "Authorization", auth_header);
    }

    if (json_body) {
        soup_message_set_request(msg, "application/json", SOUP_MEMORY_COPY, json_body, strlen(json_body));
    }

    guint status = soup_session_send_message(session, msg);
    
    char *ret = NULL;
    if (msg->response_body->length > 0) {
        ret = g_strndup(msg->response_body->data, msg->response_body->length);
    } else {
        ret = g_strdup_printf("HTTP %u %s", status, soup_status_get_phrase(status));
    }
    g_object_unref(msg);
    return ret;
}

static char *extract_lens(const char *json_str) {
    JsonParser *p = json_parser_new();
    char *lens = g_strdup("lens1"); /* default */
    if (json_parser_load_from_data(p, json_str, strlen(json_str), NULL)) {
        JsonObject *obj = json_node_get_object(json_parser_get_root(p));
        if (json_object_has_member(obj, "lens")) {
            g_free(lens);
            lens = g_strdup(json_object_get_string_member(obj, "lens"));
        }
    }
    g_object_unref(p);
    return lens;
}

static void print_help(void)
{
    g_print("\n═══ SMART IP Edge REST Mock CLI (C) ═══\n");
    g_print("Commands:\n");
    g_print("  login <user> <pass> — Login to REST API\n");
    g_print("  configure <json>    — Configure a lens\n");
    g_print("  status              — Get system status\n");
    g_print("  snapshot <lens>     — Take a snapshot\n");
    g_print("  record_start <json> — Start continuous recording\n");
    g_print("  record_stop <lens> [tier] — Stop recording on a tier\n");
    g_print("  ai_recording <lens> <on|off> — Toggle AI triggered recording\n");
    g_print("  record_schedule <json> — Start scheduled recording\n");
    g_print("  ntp <lens> <branch> <on|off> — Toggle NTP overlay\n");
    g_print("  log <level>         — Set system log level\n");
    g_print("  smtp <host> <port> <user> <pass> <from> [ssl:on|off] — Update SMTP\n");
    g_print("  smtp_get            — Get current SMTP config\n");
    g_print("  user_add <u..> <p..> <r..> <e..> — Create user\n");
    g_print("  user_list           — List all users\n");
    g_print("  user_del <user>     — Delete user\n");
    g_print("  reboot              — Reboot device\n");
    g_print("  factory_reset       — Full data wipe\n");
    g_print("  net_reset           — Restart network\n");
    g_print("  totp_setup          — Start TOTP registration\n");
    g_print("  pass_change <o> <n> — Change password\n");
    g_print("  quit                — Exit\n\n");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    session = soup_session_new();
    g_object_set(session, "ssl-strict", FALSE, NULL);
    print_help();
    char line[4096];

    while (TRUE) {
        g_print("webmock> ");
        if (!fgets(line, sizeof(line), stdin)) break;

        /* Strip newline */
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;

        char *cmd = strtok(line, " ");
        char *arg = strtok(NULL, "");

        if (g_strcmp0(cmd, "quit") == 0 || g_strcmp0(cmd, "exit") == 0) {
            break;

        } else if (g_strcmp0(cmd, "login") == 0) {
            char user[64], pass[64];
            if (!arg || sscanf(arg, "%63s %63s", user, pass) != 2) {
                g_print("Usage: login <user> <pass>\n");
                continue;
            }
            char json_body[256];
            snprintf(json_body, sizeof(json_body), "{\"username\":\"%s\",\"password\":\"%s\"}", user, pass);
            
            char *r = call_api("POST", "/auth/login", json_body);
            
            /* Stage 1: Check status */
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, r, strlen(r), NULL)) {
                JsonObject *obj = json_node_get_object(json_parser_get_root(p));
                
                if (json_object_has_member(obj, "token")) {
                    if (auth_token) g_free(auth_token);
                    auth_token = g_strdup(json_object_get_string_member(obj, "token"));
                    g_print("✓ Login successful. Token acquired.\n");
                } 
                else if (g_strcmp0(json_object_get_string_member_with_default(obj, "status", ""), "otp_required") == 0) {
                    char otp[16];
                    g_print("  ! Email OTP required. Please check your email.\n  OTP: ");
                    if (fgets(otp, sizeof(otp), stdin)) {
                        otp[strcspn(otp, "\n")] = '\0';
                        char otp_body[256];
                        snprintf(otp_body, sizeof(otp_body), "{\"username\":\"%s\",\"otp\":\"%s\"}", user, otp);
                        g_free(r);
                        r = call_api("POST", "/auth/verify-otp", otp_body);
                        
                        /* Re-parse for token or TOTP */
                        g_object_unref(p); p = json_parser_new();
                        if (json_parser_load_from_data(p, r, strlen(r), NULL)) {
                            obj = json_node_get_object(json_parser_get_root(p));
                            if (json_object_has_member(obj, "token")) {
                                if (auth_token) g_free(auth_token);
                                auth_token = g_strdup(json_object_get_string_member(obj, "token"));
                                g_print("✓ OTP verified. Login successful.\n");
                            } else if (g_strcmp0(json_object_get_string_member_with_default(obj, "status", ""), "totp_required") == 0) {
                                goto totp_stage;
                            }
                        }
                    }
                }
                else if (g_strcmp0(json_object_get_string_member_with_default(obj, "status", ""), "totp_required") == 0) {
                    totp_stage: ;
                    char totp[16];
                    g_print("  ! TOTP required. Please check your authenticator app.\n  Code: ");
                    if (fgets(totp, sizeof(totp), stdin)) {
                        totp[strcspn(totp, "\n")] = '\0';
                        char totp_body[256];
                        snprintf(totp_body, sizeof(totp_body), "{\"username\":\"%s\",\"totp_code\":\"%s\"}", user, totp);
                        g_free(r);
                        r = call_api("POST", "/auth/verify-totp", totp_body);
                        
                        g_object_unref(p); p = json_parser_new();
                        if (json_parser_load_from_data(p, r, strlen(r), NULL)) {
                            obj = json_node_get_object(json_parser_get_root(p));
                            if (json_object_has_member(obj, "token")) {
                                if (auth_token) g_free(auth_token);
                                auth_token = g_strdup(json_object_get_string_member(obj, "token"));
                                g_print("✓ TOTP verified. Login successful.\n");
                            }
                        }
                    }
                }
            }
            g_object_unref(p);
            g_print("%s\n", r);
            g_free(r);

        } else if (g_strcmp0(cmd, "status") == 0) {
            char *r = call_api("GET", "/system/status", NULL);
            if (r) { g_print("%s\n", r); g_free(r); }

        } else if (g_strcmp0(cmd, "configure") == 0) {
            if (!arg) { g_print("Usage: configure <json>\n"); continue; }
            char *lens = extract_lens(arg);
            char path[128];
            snprintf(path, sizeof(path), "/lenses/%s/config", lens);
            
            char *r = call_api("PATCH", path, arg);
            if (r) { g_print("%s\n", r); g_free(r); }
            g_free(lens);

        } else if (g_strcmp0(cmd, "snapshot") == 0) {
            char lens[32] = "lens1";
            if (arg) sscanf(arg, "%31s", lens);
            char path[128];
            snprintf(path, sizeof(path), "/lenses/%s/snapshot", lens);
            
            char *r = call_api("POST", path, NULL);
            if (r) { g_print("%s\n", r); g_free(r); }

        } else if (g_strcmp0(cmd, "record_start") == 0) {
            char *json = arg ? arg : "{\"lens\":\"lens1\",\"branch\":\"main\"}";
            char *lens = extract_lens(json);
            char path[128];
            snprintf(path, sizeof(path), "/lenses/%s/recording/main", lens);
            
            char *r = call_api("POST", path, json);
            if (r) { g_print("%s\n", r); g_free(r); }
            g_free(lens);

        } else if (g_strcmp0(cmd, "record_stop") == 0) {
            char lens[32] = "lens1";
            char tier[32] = "main";
            if (arg) sscanf(arg, "%31s %31s", lens, tier);
            char path[128];
            snprintf(path, sizeof(path), "/lenses/%s/recording/%s", lens, tier);
            
            char *r = call_api("DELETE", path, NULL);
            if (r) { g_print("%s\n", r); g_free(r); }

        } else if (g_strcmp0(cmd, "ai_recording") == 0) {
            char lens[32], state[16];
            if (!arg || sscanf(arg, "%31s %15s", lens, state) != 2) {
                g_print("Usage: ai_recording <lens> <on|off>\n");
                continue;
            }
            char path[128];
            if (g_strcmp0(state, "on") == 0) {
                snprintf(path, sizeof(path), "/lenses/%s/recording/ai", lens);
                char json_body[128];
                snprintf(json_body, sizeof(json_body), "{\"lens\":\"%s\",\"branch\":\"ai\",\"mode\":\"event\",\"output_dir\":\"/data\"}", lens);
                char *r = call_api("POST", path, json_body);
                if (r) { g_print("%s\n", r); g_free(r); }
            } else {
                snprintf(path, sizeof(path), "/lenses/%s/recording/ai", lens);
                char *r = call_api("DELETE", path, NULL);
                if (r) { g_print("%s\n", r); g_free(r); }
            }

        } else if (g_strcmp0(cmd, "record_schedule") == 0) {
            if (!arg) { g_print("Usage: record_schedule <json>\n"); continue; }
            char *lens = extract_lens(arg);
            char path[128];
            /* We don't know the branch so try to extract it, or fallback to main */
            snprintf(path, sizeof(path), "/lenses/%s/recording/main", lens);
            char *r = call_api("POST", path, arg);
            if (r) { g_print("%s\n", r); g_free(r); }
            g_free(lens);

        } else if (g_strcmp0(cmd, "log") == 0) {
            char *level = arg ? arg : "INFO";
            char json_body[128];
            snprintf(json_body, sizeof(json_body), "{\"level\":\"%s\"}", level);
            
            char *r = call_api("PUT", "/system/log_level", json_body);
            if (r) { g_print("%s\n", r); g_free(r); }

        } else if (g_strcmp0(cmd, "ntp") == 0) {
            char lens[32], branch[32], state[16];
            if (!arg || sscanf(arg, "%31s %31s %15s", lens, branch, state) != 3) {
                g_print("Usage: ntp <lens> <branch> <on|off>\n");
                continue;
            }
            gboolean enabled = (g_strcmp0(state, "on") == 0);
            char json_body[128];
            snprintf(json_body, sizeof(json_body), "{\"lens_id\":\"%s\",\"tier_name\":\"%s\",\"ntp_on\":%s}", lens, branch, enabled ? "true" : "false");
            char path[128];
            snprintf(path, sizeof(path), "/lenses/%s/ntp_overlay", lens);
            char *r = call_api("PATCH", path, json_body);
            if (r) { g_print("%s\n", r); g_free(r); }

        } else if (g_strcmp0(cmd, "smtp_get") == 0) {
            char *r = call_api("GET", "/system/smtp", NULL);
            if (r) { g_print("%s\n", r); g_free(r); }

        } else if (g_strcmp0(cmd, "smtp") == 0) {
            char host[64], user[64], pass[64], from[64], ssl_str[16] = "on";
            int port;
            if (!arg || sscanf(arg, "%63s %d %63s %63s %63s %15s", host, &port, user, pass, from, ssl_str) < 5) {
                g_print("Usage: smtp <host> <port> <user> <pass> <from> [ssl:on|off]\n");
                continue;
            }
            gboolean use_ssl = (g_strcmp0(ssl_str, "on") == 0);
            char json_body[512];
            snprintf(json_body, sizeof(json_body),
                "{\"host\":\"%s\",\"port\":%d,\"user\":\"%s\",\"pass\":\"%s\",\"from\":\"%s\",\"use_ssl\":%s}",
                host, port, user, pass, from, use_ssl ? "true" : "false");
            
            char *r = call_api("PUT", "/system/smtp", json_body);
            if (r) { g_print("%s\n", r); g_free(r); }

        } else if (g_strcmp0(cmd, "user_add") == 0) {
            char u[64], p[64], r_role[64], e[64];
            if (!arg || sscanf(arg, "%63s %63s %63s %63s", u, p, r_role, e) != 4) {
                g_print("Usage: user_add <user> <pass> <role> <email>\n"); continue;
            }
            char body[512];
            snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\",\"role\":\"%s\",\"email\":\"%s\"}", u, p, r_role, e);
            char *res = call_api("POST", "/users", body);
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "user_list") == 0) {
            char *res = call_api("GET", "/users", NULL);
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "user_del") == 0) {
            if (!arg) { g_print("Usage: user_del <user>\n"); continue; }
            char path[128]; snprintf(path, sizeof(path), "/users/%s", arg);
            char *res = call_api("DELETE", path, NULL);
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "reboot") == 0) {
            char *res = call_api("POST", "/device/reboot", NULL);
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "factory_reset") == 0) {
            char *res = call_api("POST", "/device/factory-reset", "{\"confirm\":true}");
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "net_reset") == 0) {
            char *res = call_api("POST", "/device/network-reset", NULL);
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "totp_setup") == 0) {
            char *res = call_api("POST", "/auth/setup-totp", NULL);
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "pass_change") == 0) {
            char o[64], n[64];
            if (!arg || sscanf(arg, "%63s %63s", o, n) != 2) {
                g_print("Usage: pass_change <old> <new>\n"); continue;
            }
            char body[256]; snprintf(body, sizeof(body), "{\"old_password\":\"%s\",\"new_password\":\"%s\"}", o, n);
            char *res = call_api("POST", "/auth/change-password", body);
            if (res) { g_print("%s\n", res); g_free(res); }

        } else if (g_strcmp0(cmd, "help") == 0) {

            print_help();

        } else {
            g_print("Unknown command: %s (type 'help')\n", cmd);
        }
    }

    if (auth_token) g_free(auth_token);
    g_object_unref(session);
    g_print("Web Mock CLI exited.\n");
    return 0;
}
