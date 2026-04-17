#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gst/gst.h>
#include <json-glib/json-glib.h>
#include <zmq.h>

/* Json-GLib Stubs */
JsonParser *json_parser_new(void) { return NULL; }
gboolean json_parser_load_from_data(JsonParser *parser, const gchar *data, gssize length, GError **error) { return FALSE; }
JsonNode *json_parser_get_root(JsonParser *parser) { return NULL; }
JsonObject *json_node_get_object(JsonNode *node) { return NULL; }
const gchar *json_object_get_string_member(JsonObject *object, const gchar *member_name) { return NULL; }
gint64 json_object_get_int_member(JsonObject *object, const gchar *member_name) { return 0; }

/* GStreamer Stubs (that aren't already wrapped) */
GstElementFactory *gst_element_factory_find(const gchar *name) { return NULL; }
GstCaps *gst_caps_from_string(const gchar *string) { return NULL; }
void gst_caps_unref(GstCaps *caps) {}
GstElementFactory *gst_element_get_factory(GstElement *element) { return NULL; }
const gchar *gst_plugin_feature_get_name(GstPluginFeature *feature) { return ""; }
GstStructure *gst_structure_new(const gchar *name, const gchar *first_field, ...) { return NULL; }
void gst_structure_free(GstStructure *structure) {}
void gst_object_unref(gpointer object) {}
const gchar *GST_ELEMENT_NAME(GstElement *element) { return "mock_elem"; }

/* ZMQ Stubs */
void *zmq_ctx_new(void) { return NULL; }
int zmq_ctx_destroy(void *context) { return 0; }
void *zmq_socket(void *context, int type) { return NULL; }
int zmq_close(void *socket) { return 0; }
int zmq_bind(void *socket, const char *endpoint) { return 0; }
int zmq_connect(void *socket, const char *endpoint) { return 0; }
int zmq_setsockopt(void *socket, int option, const void *optval, size_t optvallen) { return 0; }
int zmq_send(void *socket, const void *buf, size_t len, int flags) { return 0; }
int zmq_recv(void *socket, void *buf, size_t len, int flags) { return 0; }
int zmq_errno(void) { return 0; }
const char *zmq_strerror(int errnum) { return ""; }
