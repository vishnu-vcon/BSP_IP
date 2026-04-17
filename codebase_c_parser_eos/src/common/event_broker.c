/*
 * event_broker.c — ZeroMQ Pub/Sub Event Broker
 * ==============================================
 * Port of: common/event_broker.py
 *
 * Exact same pattern: PUB binds, SUB connects.
 * Messages are multipart: [topic_bytes, json_bytes]
 */

#include "event_broker.h"
#include "config.h"
#include <zmq.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════
 *  Publisher — Port of MQPublisher class
 * ══════════════════════════════════════════════════════════════ */

MQPublisher *mq_publisher_new(int port)
{
    MQPublisher *pub = g_new0(MQPublisher, 1);
    pub->port = port;
    pub->zmq_ctx = zmq_ctx_new();
    pub->zmq_socket = zmq_socket(pub->zmq_ctx, ZMQ_PUB);

    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%d", port);
    int rc = zmq_bind(pub->zmq_socket, endpoint);
    if (rc != 0) {
        g_warning("ZeroMQ Publisher failed to bind to port %d: %s", port, zmq_strerror(zmq_errno()));
    } else {
        g_debug("ZeroMQ Publisher bound to port %d", port);
    }
    return pub;
}

void mq_publisher_publish(MQPublisher *pub, const char *topic, const char *json_payload)
{
    if (!pub || !pub->zmq_socket) return;

    /* Send multipart: [topic, json_payload] — matches Python version */
    zmq_send(pub->zmq_socket, topic, strlen(topic), ZMQ_SNDMORE);
    zmq_send(pub->zmq_socket, json_payload, strlen(json_payload), 0);

    g_debug("MQ [PUB]: Message sent to topic '%s'", topic);
}

void mq_publisher_free(MQPublisher *pub)
{
    if (!pub) return;
    if (pub->zmq_socket) zmq_close(pub->zmq_socket);
    if (pub->zmq_ctx)    zmq_ctx_destroy(pub->zmq_ctx);
    g_free(pub);
}

/* ══════════════════════════════════════════════════════════════
 *  Subscriber — Port of MQSubscriber class
 * ══════════════════════════════════════════════════════════════ */

MQSubscriber *mq_subscriber_new(int port)
{
    MQSubscriber *sub = g_new0(MQSubscriber, 1);
    sub->port = port;
    sub->running = FALSE;
    sub->zmq_ctx = zmq_ctx_new();
    sub->zmq_socket = zmq_socket(sub->zmq_ctx, ZMQ_SUB);

    /* Set receive timeout to 1000ms so the loop can check sub->running */
    int timeout_ms = 1000;
    zmq_setsockopt(sub->zmq_socket, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%d", port);
    zmq_connect(sub->zmq_socket, endpoint);

    return sub;
}

void mq_subscriber_subscribe(MQSubscriber *sub, const char *topic)
{
    if (!sub || !sub->zmq_socket) return;
    zmq_setsockopt(sub->zmq_socket, ZMQ_SUBSCRIBE, topic, strlen(topic));
    g_debug("ZeroMQ Subscribed to: %s", topic);
}

void mq_subscriber_set_callback(MQSubscriber *sub, MQCallback cb, gpointer user_data)
{
    if (!sub) return;
    sub->callback = cb;
    sub->callback_data = user_data;
}

/*
 * _listener_thread_func — Background loop (port of listen_forever)
 * Runs in a GThread, receives multipart [topic, json] and triggers callback.
 */
static gpointer _listener_thread_func(gpointer data)
{
    MQSubscriber *sub = (MQSubscriber *)data;
    char topic_buf[256];
    char msg_buf[8192];

    g_debug("ZeroMQ Listener started...");

    while (sub->running) {
        /* Receive topic part */
        int topic_len = zmq_recv(sub->zmq_socket, topic_buf, sizeof(topic_buf) - 1, 0);
        if (topic_len < 0) {
            if (zmq_errno() == EAGAIN) continue; /* Normal timeout */
            g_warning("MQ Listener Error: %s", zmq_strerror(zmq_errno()));
            continue;
        }
        topic_buf[topic_len] = '\0';

        /* Receive message part */
        int msg_len = zmq_recv(sub->zmq_socket, msg_buf, sizeof(msg_buf) - 1, 0);
        if (msg_len < 0) continue;
        msg_buf[msg_len] = '\0';

        g_debug("MQ [SUB]: Received on topic '%s': %s", topic_buf, msg_buf);

        if (sub->callback) {
            sub->callback(topic_buf, msg_buf, sub->callback_data);
        }
    }

    return NULL;
}

void mq_subscriber_start_background(MQSubscriber *sub)
{
    if (!sub) return;
    sub->running = TRUE;
    sub->listener_thread = g_thread_new("zmq-listener", _listener_thread_func, sub);
}

void mq_subscriber_stop(MQSubscriber *sub)
{
    if (!sub) return;
    sub->running = FALSE;
    if (sub->listener_thread) {
        g_thread_join(sub->listener_thread);
        sub->listener_thread = NULL;
    }
    g_debug("ZeroMQ Subscriber stopped.");
}

void mq_subscriber_free(MQSubscriber *sub)
{
    if (!sub) return;
    mq_subscriber_stop(sub);
    if (sub->zmq_socket) zmq_close(sub->zmq_socket);
    if (sub->zmq_ctx)    zmq_ctx_destroy(sub->zmq_ctx);
    g_free(sub);
}
