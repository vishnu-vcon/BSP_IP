/*
 * event_broker.h — ZeroMQ Pub/Sub Event Broker
 * ==============================================
 * Port of: common/event_broker.py
 *
 * Provides MQPublisher and MQSubscriber for decoupled
 * inter-process event messaging.
 */

#ifndef SMARTIP_EVENT_BROKER_H
#define SMARTIP_EVENT_BROKER_H

#include <glib.h>
#include <json-glib/json-glib.h>

/* ── Publisher ── */
typedef struct {
    void *zmq_ctx;
    void *zmq_socket;
    int   port;
} MQPublisher;

MQPublisher *mq_publisher_new     (int port);
void         mq_publisher_publish (MQPublisher *pub, const char *topic, const char *json_payload);
void         mq_publisher_free    (MQPublisher *pub);

/* ── Subscriber ── */
typedef void (*MQCallback)(const char *topic, const char *json_payload, gpointer user_data);

typedef struct {
    void       *zmq_ctx;
    void       *zmq_socket;
    int         port;
    GThread    *listener_thread;
    gboolean    running;
    MQCallback  callback;
    gpointer    callback_data;
} MQSubscriber;

MQSubscriber *mq_subscriber_new             (int port);
void          mq_subscriber_subscribe       (MQSubscriber *sub, const char *topic);
void          mq_subscriber_set_callback    (MQSubscriber *sub, MQCallback cb, gpointer user_data);
void          mq_subscriber_start_background(MQSubscriber *sub);
void          mq_subscriber_stop            (MQSubscriber *sub);
void          mq_subscriber_free            (MQSubscriber *sub);

#endif /* SMARTIP_EVENT_BROKER_H */
