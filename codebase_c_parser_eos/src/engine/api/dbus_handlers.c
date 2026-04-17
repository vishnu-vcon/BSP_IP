/*
 * dbus_service.c — D-Bus Service Implementation (GDBus)
 * ======================================================
 */

#include "api/dbus_handlers.h"
#include "../common/config.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

#include "engine_types.h"

extern char *engine_configure_lens   (UnifiedEngine *engine, const char *json);
extern char *engine_update_stream    (UnifiedEngine *engine, const char *lens, const char *branch, const char *json);
extern char *engine_stop_lens        (UnifiedEngine *engine, const char *json);
extern char *engine_take_snapshot    (UnifiedEngine *engine, const char *lens, const char *path);
extern char *engine_start_recording  (UnifiedEngine *engine, const char *json);
extern char *engine_stop_recording   (UnifiedEngine *engine, const char *json);
extern char *engine_start_hls        (UnifiedEngine *engine, const char *json);
extern char *engine_stop_hls         (UnifiedEngine *engine, const char *json);
extern char *engine_get_status       (UnifiedEngine *engine);
extern char *engine_set_log_level    (UnifiedEngine *engine, const char *level);
extern char *engine_attach_shm       (UnifiedEngine *engine, const char *json);
extern char *engine_detach_shm       (UnifiedEngine *engine, const char *json);
extern char *engine_toggle_overlay   (UnifiedEngine *engine, const char *json);
extern char *engine_toggle_ntp       (UnifiedEngine *engine, const char *json);
extern gboolean engine_save_config   (UnifiedEngine *engine);

/* ── Method Call Handler ── */
static void _handle_method_call(GDBusConnection       *connection,
                                 const gchar           *sender,
                                 const gchar           *object_path,
                                 const gchar           *interface_name,
                                 const gchar           *method_name,
                                 GVariant              *parameters,
                                 GDBusMethodInvocation *invocation,
                                 gpointer               user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name;
    UnifiedEngine *engine = (UnifiedEngine *)user_data;
    char *result_str = NULL;
    gboolean is_void_call = FALSE;

    if (g_strcmp0(method_name, "ConfigureLens") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_configure_lens(engine, json_params);

    } else if (g_strcmp0(method_name, "UpdateStreamParams") == 0) {
        const gchar *lens_name, *branch_name, *json_params;
        g_variant_get(parameters, "(&s&s&s)", &lens_name, &branch_name, &json_params);
        result_str = engine_update_stream(engine, lens_name, branch_name, json_params);

    } else if (g_strcmp0(method_name, "StopLens") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_stop_lens(engine, json_params);

    } else if (g_strcmp0(method_name, "TakeSnapshot") == 0) {
        const gchar *lens_name, *snapshot_path;
        g_variant_get(parameters, "(&s&s)", &lens_name, &snapshot_path);
        result_str = engine_take_snapshot(engine, lens_name, snapshot_path);

    } else if (g_strcmp0(method_name, "StartRecording") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_start_recording(engine, json_params);

    } else if (g_strcmp0(method_name, "StopRecording") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_stop_recording(engine, json_params);

    } else if (g_strcmp0(method_name, "StartHLS") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_start_hls(engine, json_params);

    } else if (g_strcmp0(method_name, "StopHLS") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_stop_hls(engine, json_params);

    } else if (g_strcmp0(method_name, "GetStatus") == 0) {
        result_str = engine_get_status(engine);

    } else if (g_strcmp0(method_name, "SetLogLevel") == 0) {
        const gchar *log_level;
        g_variant_get(parameters, "(&s)", &log_level);
        result_str = engine_set_log_level(engine, log_level);

    } else if (g_strcmp0(method_name, "AttachSHM") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_attach_shm(engine, json_params);

    } else if (g_strcmp0(method_name, "DetachSHM") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_detach_shm(engine, json_params);

    } else if (g_strcmp0(method_name, "ToggleOverlay") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_toggle_overlay(engine, json_params);

    } else if (g_strcmp0(method_name, "ToggleNTP") == 0) {
        const gchar *json_params;
        g_variant_get(parameters, "(&s)", &json_params);
        result_str = engine_toggle_ntp(engine, json_params);

    } else if (g_strcmp0(method_name, "SaveCurrentConfig") == 0) {
        gboolean save_success = engine_save_config(engine);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", save_success));
        is_void_call = TRUE;

    } else {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "com.camera.UnifiedEngine.UnknownMethod",
            "Method not implemented");
        is_void_call = TRUE;
    }

    /* Safe UTF-8 handling and return */
    if (!is_void_call) {
        if (!result_str) result_str = g_strdup("");
        
        if (!g_utf8_validate(result_str, -1, NULL)) {
            g_warning("D-Bus result for %s was not valid UTF-8! Sanitizing...", method_name);
            char *valid_utf8 = g_utf8_make_valid(result_str, -1);
            g_free(result_str);
            result_str = valid_utf8;
        }

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", result_str));
    }
    
    g_free(result_str);
}

static const GDBusInterfaceVTable _vtable = {
    .method_call  = _handle_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

static void _on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    (void)user_data;
    g_info("D-Bus name acquired: %s", name);
}

static void _on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    (void)user_data;
    g_warning("D-Bus name lost: %s", name);
}

static void _on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)name;
    GError *error = NULL;
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(UNIFIED_IFACE_XML, &error);
    if (!node_info) {
        g_error("Failed to parse D-Bus introspection XML: %s", error->message);
        return;
    }

    g_dbus_connection_register_object(connection, UNIFIED_BUS_PATH, node_info->interfaces[0], &_vtable, user_data, NULL, &error);
    if (error) {
        g_error("Failed to register D-Bus object: %s", error->message);
        g_error_free(error);
    }
    g_dbus_node_info_unref(node_info);
}

guint dbus_service_start(UnifiedEngine *engine) {
    return g_bus_own_name(G_BUS_TYPE_SESSION, UNIFIED_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_REPLACE, _on_bus_acquired, _on_name_acquired, _on_name_lost, engine, NULL);
}
