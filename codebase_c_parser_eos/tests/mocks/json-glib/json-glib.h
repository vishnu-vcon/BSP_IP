#ifndef MOCK_JSON_GLIB_H
#define MOCK_JSON_GLIB_H

#include <glib.h>
#include <glib-object.h>

typedef struct _JsonParser JsonParser;
typedef struct _JsonNode JsonNode;
typedef struct _JsonObject JsonObject;

JsonParser *json_parser_new(void);
gboolean json_parser_load_from_data(JsonParser *parser, const gchar *data, gssize length, GError **error);
JsonNode *json_parser_get_root(JsonParser *parser);
JsonObject *json_node_get_object(JsonNode *node);
const gchar *json_object_get_string_member(JsonObject *object, const gchar *member_name);
gint64 json_object_get_int_member(JsonObject *object, const gchar *member_name);

#endif
