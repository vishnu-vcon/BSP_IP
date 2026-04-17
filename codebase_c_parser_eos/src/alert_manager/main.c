/*
 * main.c — Alert Manager (Binary 3)
 * ====================================
 * Port of: alert_manager.py (63 lines)
 *
 * Process 3: ZMQ subscriber that logs AI violation alerts.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zmq.h>

#include "../common/config.h"
#include "../common/event_broker.h"

typedef struct {
    double   timestamp;
    char     topic[64];
    char     data[4096];
} AlertEntry;

typedef struct {
    MQSubscriber *subscriber;
    AlertEntry    history[100];
    int           history_count;
    int           history_head;
} AlertManager;

static AlertManager *g_am = NULL;

static void _on_alert(const char *topic, const char *json, gpointer user_data)
{
    AlertManager *am = (AlertManager *)user_data;

    /* Parse for display */
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, json, -1, NULL)) {
        JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
        const char *type = json_object_get_string_member_with_default(obj, "type", "unknown");
        gdouble score = json_object_get_double_member_with_default(obj, "score", 0.0);
        g_info("🚨 ALARM TRIGGERED 🚨: Type: %s | Score: %.2f", type, score);

        if (json_object_has_member(obj, "snapshot")) {
            g_info("📸 Snapshot: %s", json_object_get_string_member(obj, "snapshot"));
        }
    }
    g_object_unref(parser);

    /* Store in circular buffer */
    int idx = am->history_head % 100;
    am->history[idx].timestamp = (double)time(NULL);
    g_strlcpy(am->history[idx].topic, topic, sizeof(am->history[idx].topic));
    g_strlcpy(am->history[idx].data, json, sizeof(am->history[idx].data));
    am->history_head++;
    if (am->history_count < 100) am->history_count++;
}

static void _signal_handler(int signum)
{
    (void)signum;
    if (g_am) mq_subscriber_stop(g_am->subscriber);
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    g_info("Starting Alert Manager service (C)...");

    AlertManager am = {0};
    am.subscriber = mq_subscriber_new(ZMQ_DEFAULT_PORT);
    mq_subscriber_subscribe(am.subscriber, TOPIC_ALERTS);
    mq_subscriber_set_callback(am.subscriber, _on_alert, &am);

    g_am = &am;
    signal(SIGINT, _signal_handler);
    signal(SIGTERM, _signal_handler);

    /* listen_forever blocks until stopped */
    am.subscriber->running = TRUE;
    g_info("Alert Manager listening on ZMQ port %d...", ZMQ_DEFAULT_PORT);

    /* Inline listen loop (simpler than background thread for standalone) */
    char topic_buf[256], msg_buf[8192];
    while (am.subscriber->running) {
        int topic_len = zmq_recv(am.subscriber->zmq_socket, topic_buf, sizeof(topic_buf) - 1, 0);
        if (topic_len < 0) {
            if (zmq_errno() == EAGAIN) continue;
            break;
        }
        topic_buf[topic_len] = '\0';

        int msg_len = zmq_recv(am.subscriber->zmq_socket, msg_buf, sizeof(msg_buf) - 1, 0);
        if (msg_len < 0) continue;
        msg_buf[msg_len] = '\0';

        _on_alert(topic_buf, msg_buf, &am);
    }

    mq_subscriber_free(am.subscriber);
    g_print("Alert Manager shut down.\n");
    return 0;
}
