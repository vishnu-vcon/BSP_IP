/*
 * core/http_server.h — HTTPS REST API Server (Merged v1+v2)
 * ===========================================================
 *
 * New in merge:
 *   - http_server_authorize_ex() — authorise and extract user/role
 *   - /api/v1/auth/...  handlers: verify-otp, verify-totp, setup-totp,
 *                                logout, change-password, forgot-password,
 *                                reset-password
 *   - /api/v1/users    handler:  list / add / delete users (admin)
 *   - /api/v1/device   handler:  reboot, status, network-reset,
 *                                config-reset, factory-reset  (v1 device_controls)
 */

#ifndef SMARTIP_HTTP_SERVER_H
#define SMARTIP_HTTP_SERVER_H

#include <glib.h>
#include <libsoup/soup.h>
#include "core/auth.h"

typedef struct _HTTPServer HTTPServer;

HTTPServer  *http_server_new (TokenAuth *auth, int port, gboolean enable_tls);
void         http_server_start(HTTPServer *srv);
void         http_server_stop (HTTPServer *srv);
void         http_server_free (HTTPServer *srv);

SoupServer  *http_server_get_soup(HTTPServer *srv);

/* ── Authorisation utilities ─────────────────────────────────────────── */

/*
 * Validate the Bearer token from the request and check role hierarchy.
 * Sends 401/403 automatically on failure.
 * Returns TRUE if authorised.
 */
gboolean http_server_authorize(HTTPServer   *srv,
                               SoupMessage  *msg,
                               GHashTable   *query,
                               const char   *required_role);

/*
 * Same as http_server_authorize() but also writes the authenticated
 * username and role into out_user / out_role (caller-provided buffers).
 * Use this wherever you need the identity for logging or passing to
 * v1 device_controls / activity_logger.
 */
gboolean http_server_authorize_ex(HTTPServer   *srv,
                                  SoupMessage  *msg,
                                  GHashTable   *query,
                                  const char   *required_role,
                                  char         *out_user, int user_len,
                                  char         *out_role, int role_len);

/* ── JSON response helpers ────────────────────────────────────────────── */

void http_server_respond_json (SoupMessage *msg, int status, const char *json);
void http_server_respond_error(SoupMessage *msg, int status, const char *err_msg);

/* ── Shared handler (used by main.c) ─────────────────────────────────── */

void _handle_static_recordings(SoupServer       *server,
                                SoupMessage      *msg,
                                const char       *path,
                                GHashTable       *query,
                                SoupClientContext *client,
                                gpointer          user_data);

#endif /* SMARTIP_HTTP_SERVER_H */
