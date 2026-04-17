/*
 * hls_test_server.c — Standalone HLS Test Server with Embedded Player
 * ====================================================================
 * A lightweight HTTP server for testing HLS streams independently.
 * The player UI is embedded directly — no external files needed.
 *
 * Build (on-board):
 *   gcc -O2 -o smartip_hls_test src/tools/hls_test_server.c \
 *       $(pkg-config --cflags --libs libsoup-2.4)
 *
 * Usage:
 *   ./smartip_hls_test                         # default port 8098
 *   ./smartip_hls_test -p 9000                 # custom port
 *
 * Access:
 *   Player:  http://<board-ip>:8098/player.html?stream=lens1/main
 *   Stream:  http://<board-ip>:8098/lens1/main/playlist.m3u8
 */

#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_PORT 8098
#define HLS_ROOT "/tmp/hls"

/* ── Embedded Player HTML ── */
static const char *PLAYER_HTML =
"<!DOCTYPE html>\n"
"<html lang='en'>\n"
"<head>\n"
"  <meta charset='UTF-8'>\n"
"  <meta name='viewport' content='width=device-width, initial-scale=1.0'>\n"
"  <title>SmartIP Edge - HLS Stream Tester</title>\n"
"  <script src='https://cdn.jsdelivr.net/npm/hls.js@latest'></script>\n"
"  <style>\n"
"    :root { --bg: #0f172a; --card: #1e293b; --accent: #10b981; --text: #f8fafc; }\n"
"    * { box-sizing: border-box; margin: 0; padding: 0; }\n"
"    body { font-family: -apple-system, system-ui, sans-serif; background: var(--bg); color: var(--text);\n"
"           display: flex; justify-content: center; padding: 40px 20px; min-height: 100vh; }\n"
"    .container { width: 100%%; max-width: 900px; }\n"
"    h1 { font-size: 1.5rem; font-weight: 700; color: var(--accent); margin-bottom: 8px; }\n"
"    p.sub { color: #94a3b8; font-size: 0.875rem; margin-bottom: 24px; }\n"
"    .controls { display: flex; gap: 10px; margin-bottom: 20px; flex-wrap: wrap; }\n"
"    select, button { background: var(--card); border: 1px solid #334155; color: white;\n"
"                     padding: 10px 16px; border-radius: 8px; font-size: 0.875rem; }\n"
"    button { background: var(--accent); border-color: var(--accent); cursor: pointer; font-weight: 600; }\n"
"    button:hover { opacity: 0.9; }\n"
"    .video-wrap { background: black; border-radius: 12px; overflow: hidden; aspect-ratio: 16/9;\n"
"                  border: 1px solid #334155; }\n"
"    video { width: 100%%; height: 100%%; }\n"
"    #status { margin-top: 12px; font-family: monospace; font-size: 0.8rem; padding: 10px;\n"
"              background: var(--card); border-radius: 8px; color: var(--accent); }\n"
"    #status.error { color: #f43f5e; }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div class='container'>\n"
"    <h1>SmartIP Edge — HLS Stream Tester</h1>\n"
"    <p class='sub'>Standalone test player. No authentication required.</p>\n"
"    <div class='controls'>\n"
"      <select id='stream'>\n"
"        <option value='lens1/main'>lens1 / main</option>\n"
"        <option value='lens1/sub'>lens1 / sub</option>\n"
"        <option value='lens2/main'>lens2 / main</option>\n"
"        <option value='lens2/sub'>lens2 / sub</option>\n"
"      </select>\n"
"      <button onclick='playStream()'>▶ Play</button>\n"
"    </div>\n"
"    <div class='video-wrap'>\n"
"      <video id='video' controls muted playsinline></video>\n"
"    </div>\n"
"    <div id='status'>Select a stream and press Play.</div>\n"
"  </div>\n"
"  <script>\n"
"    // Pre-select from query param\n"
"    const params = new URLSearchParams(window.location.search);\n"
"    const initStream = params.get('stream');\n"
"    if (initStream) {\n"
"      const sel = document.getElementById('stream');\n"
"      for (let o of sel.options) { if (o.value === initStream) o.selected = true; }\n"
"    }\n"
"\n"
"    let hlsPlayer = null;\n"
"    function playStream() {\n"
"      const stream = document.getElementById('stream').value;\n"
"      const video = document.getElementById('video');\n"
"      const status = document.getElementById('status');\n"
"      const host = window.location.hostname || '127.0.0.1';\n"
"      const port = window.location.port || '8098';\n"
"      const url = 'http://' + host + ':' + port + '/' + stream + '/playlist.m3u8';\n"
"\n"
"      status.className = 'status';\n"
"      status.innerText = 'Connecting to: ' + url + '...';\n"
"\n"
"      if (hlsPlayer) hlsPlayer.destroy();\n"
"\n"
"      if (Hls.isSupported()) {\n"
"        hlsPlayer = new Hls({ liveSyncDurationCount: 1, liveMaxLatencyDurationCount: 3, maxBufferLength: 5 });\n"
"        hlsPlayer.loadSource(url);\n"
"        hlsPlayer.attachMedia(video);\n"
"        hlsPlayer.on(Hls.Events.MANIFEST_PARSED, function() {\n"
"          video.play();\n"
"          status.innerText = '✅ LIVE: ' + stream;\n"
"        });\n"
"        hlsPlayer.on(Hls.Events.ERROR, function(event, data) {\n"
"          status.className = 'error';\n"
"          status.innerText = '❌ ' + data.details;\n"
"        });\n"
"      } else if (video.canPlayType('application/vnd.apple.mpegurl')) {\n"
"        video.src = url;\n"
"        status.innerText = 'Using native HLS';\n"
"      }\n"
"    }\n"
"    // Auto-play if stream param given\n"
"    if (initStream) playStream();\n"
"  </script>\n"
"</body>\n"
"</html>\n";

/* ── Request Handler ── */
static void handle_request(SoupServer *server, SoupMessage *msg, const char *path,
                           GHashTable *query, SoupClientContext *client, gpointer user_data)
{
    (void)server; (void)query; (void)client; (void)user_data;

    /* CORS */
    soup_message_headers_append(msg->response_headers, "Access-Control-Allow-Origin", "*");
    soup_message_headers_append(msg->response_headers, "Access-Control-Allow-Methods", "GET, OPTIONS");

    if (msg->method == SOUP_METHOD_OPTIONS) {
        soup_message_set_status(msg, SOUP_STATUS_OK);
        return;
    }
    if (msg->method != SOUP_METHOD_GET) {
        soup_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }

    /* Root → redirect to player */
    if (g_strcmp0(path, "/") == 0) {
        soup_message_headers_append(msg->response_headers, "Location", "/player.html");
        soup_message_set_status(msg, 302);
        return;
    }

    /* Serve embedded player */
    if (g_strcmp0(path, "/player.html") == 0) {
        soup_message_set_response(msg, "text/html", SOUP_MEMORY_STATIC,
                                  (gchar *)PLAYER_HTML, strlen(PLAYER_HTML));
        soup_message_set_status(msg, SOUP_STATUS_OK);
        return;
    }

    /* Serve HLS files from /tmp/hls/ */
    printf("Request: %s\n", path);
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", HLS_ROOT, path);

    if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
        soup_message_set_status(msg, SOUP_STATUS_FORBIDDEN);
        return;
    }

    gchar *contents;
    gsize length;
    GError *error = NULL;

    if (g_file_get_contents(full_path, &contents, &length, &error)) {
        const char *mime = "application/octet-stream";
        const char *ext = strrchr(path, '.');
        if (ext) {
            if (g_strcmp0(ext, ".m3u8") == 0) mime = "application/vnd.apple.mpegurl";
            else if (g_strcmp0(ext, ".ts") == 0) mime = "video/mp2t";
        }

        soup_message_headers_append(msg->response_headers, "Cache-Control", "no-cache, no-store, must-revalidate");
        soup_message_set_response(msg, mime, SOUP_MEMORY_TAKE, contents, length);
        soup_message_set_status(msg, SOUP_STATUS_OK);
        printf("  -> Served %s (%zu bytes)\n", path, length);
    } else {
        printf("  -> 404: %s\n", error->message);
        soup_message_set_status(msg, SOUP_STATUS_NOT_FOUND);
        g_error_free(error);
    }
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;

    /* Simple arg parsing */
    for (int i = 1; i < argc; i++) {
        if ((g_strcmp0(argv[i], "-p") == 0 || g_strcmp0(argv[i], "--port") == 0) && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    SoupServer *server = soup_server_new(NULL, NULL);
    soup_server_add_handler(server, NULL, handle_request, NULL, NULL);

    GError *error = NULL;
    if (!soup_server_listen_all(server, port, 0, &error)) {
        fprintf(stderr, "Unable to listen on port %d: %s\n", port, error->message);
        return 1;
    }

    printf("═══════════════════════════════════════\n");
    printf(" SmartIP Edge — HLS Test Server\n");
    printf("═══════════════════════════════════════\n");
    printf(" Port:   %d\n", port);
    printf(" Root:   %s\n", HLS_ROOT);
    printf(" Player: http://0.0.0.0:%d/player.html\n", port);
    printf("═══════════════════════════════════════\n");

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    return 0;
}
