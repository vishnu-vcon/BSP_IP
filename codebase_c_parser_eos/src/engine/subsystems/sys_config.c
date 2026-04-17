/*
 * sys_config.c — Configuration Subsystem Implementation
 * ======================================================
 */

#include "sys_config.h"
#include "../../common/config.h"
#include <stdio.h>
#include <string.h>

/* Internal helper: Validates and loads a single JSON file */
static gboolean _try_load_config(const char *path, gchar **contents, gsize *len) {
    if (!path || path[0] == '\0') {
        return FALSE;
    }

    GError *err = NULL;
    if (!g_file_get_contents(path, contents, len, &err)) {
        if (err) {
            g_debug("[CONFIG] Skipping %s: %s", path, err->message);
            g_error_free(err);
        }
        return FALSE;
    }

    /* Aggressive NULL check on loaded content */
    if (!*contents || *len == 0) {
        g_warning("[CONFIG] File %s read successfully but is empty.", path);
        g_free(*contents);
        *contents = NULL;
        return FALSE;
    }

    /* Structural validation: Ensure it's valid JSON */
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, *contents, *len, NULL)) {
        g_warning("[CONFIG] File %s contains malformed JSON. Skipping...", path);
        g_free(*contents);
        *contents = NULL;
        g_object_unref(parser);
        return FALSE;
    }

    g_object_unref(parser);
    return TRUE;
}

static JsonObject *_config_get_active(JsonObject *root) {
    if (!root) return NULL;
    
    const char *tiers[] = {"user_overrides", "manufacturer_defaults", NULL};
    for (int i = 0; tiers[i]; i++) {
        if (json_object_has_member(root, tiers[i])) {
            JsonNode *node = json_object_get_member(root, tiers[i]);
            if (node && json_node_get_node_type(node) == JSON_NODE_OBJECT) {
                return json_node_get_object(node);
            }
        }
    }
    
    /* If neither wrappers exist or they are null, assume it's a flat config */
    return root;
}

static gboolean _config_is_enabled(JsonObject *root) {
    if (!root) return FALSE;
    if (json_object_has_member(root, "enabled")) {
        return json_object_get_boolean_member(root, "enabled");
    }
    return TRUE; /* Default to enabled if key missing */
}

void sys_config_load_and_apply(UnifiedEngine *e) {
    if (!e) {
        g_error("[CONFIG] Critical Error: sys_config_load_and_apply called with NULL engine.");
        return;
    }

    gchar *contents = NULL;
    gsize len = 0;
    const char *source = NULL;

    /* ── TIER 1: CLI Provided Path ── */
    /* If the path is NOT the default user_config, it must be a CLI override */
    if (g_strcmp0(e->config_path, "config/user_config.json") != 0) {
        if (_try_load_config(e->config_path, &contents, &len)) {
            source = e->config_path;
        }
    }

    /* ── TIER 2: User Persistence ── */
    if (!contents && _try_load_config("config/user_config.json", &contents, &len)) {
        source = "config/user_config.json";
    }

    /* ── TIER 2.5: Power-Loss Backup ──────────────────────────────────────────
     * [FIX-2] If user_config.json is present but contains corrupt or empty
     * JSON (e.g. the filesystem wrote the file header but lost the data block
     * during a sudden power-off on a non-journalled FS), _try_load_config
     * rejects it above and we fall here.  _save_user_config_unlocked rotates
     * the previous known-good file to user_config.json.bak immediately before
     * every atomic write, so .bak always holds the last successfully-committed
     * configuration.  Falling back to it avoids the "soft-brick" state where
     * the engine starts with no streams configured at all. */
    if (!contents && _try_load_config("config/user_config.json.bak", &contents, &len)) {
        source = "config/user_config.json.bak";
        g_warning("[CONFIG] Primary user config is corrupt or missing — "
                  "recovered from backup '%s'. "
                  "The engine will re-save a clean copy on first state change.",
                  source);
    }

    /* ── TIER 3: Manufacturer Defaults ── */
    if (!contents && _try_load_config(DEFAULT_CONFIG_PATH, &contents, &len)) {
        source = DEFAULT_CONFIG_PATH;
    }

    if (!contents) {
        g_warning("[CONFIG] No valid configuration found in any of the 3 tiers. System starting with default idle state.");
        return;
    }

    g_info("[CONFIG] RESTORATION: Successfully loaded configuration from %s", source);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, contents, len, NULL)) {
        /* This should not happen due to _try_load_config check, but safety first */
        g_free(contents);
        g_object_unref(parser);
        return;
    }
    g_free(contents);

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (!root || !_config_is_enabled(root)) {
        g_info("[CONFIG] Configuration state is DISABLED in file. Skipping auto-start.");
        g_object_unref(parser);
        return;
    }

    JsonObject *active = _config_get_active(root);
    if (!active) {
        g_warning("[CONFIG] Could not find 'user_overrides' or 'manufacturer_defaults' in config.");
        g_object_unref(parser);
        return;
    }

    /* Iterate over lenses in config */
    GList *lens_ids = json_object_get_members(active);
    for (GList *l = lens_ids; l; l = l->next) {
        const char *lid = (const char *)l->data;
        if (!lid) continue;

        if (!g_hash_table_contains(e->lenses, lid)) {
            g_debug("[CONFIG] Skipping unknown lens ID in config: %s", lid);
            continue;
        }

        JsonObject *lens_cfg = json_object_get_object_member(active, lid);
        if (!lens_cfg) continue;

        /* ───── Build Reconstruction Command ───── */
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "lens");
        json_builder_add_string_value(b, lid);

        /* [ALIGNMENT FIX]: Explicitly forward lens-level attributes */
        if (json_object_has_member(lens_cfg, "cairo")) {
            json_builder_set_member_name(b, "cairo");
            json_builder_add_boolean_value(b, json_object_get_boolean_member(lens_cfg, "cairo"));
        }
        if (json_object_has_member(lens_cfg, "overlay")) {
            json_builder_set_member_name(b, "overlay");
            json_builder_add_boolean_value(b, json_object_get_boolean_member(lens_cfg, "overlay"));
        }

        gboolean found_streams = FALSE;

        /* Logic: Check for a nested "rtsp" object OR direct "main pack" members */
        if (json_object_has_member(lens_cfg, "rtsp")) {
            JsonObject *rtsp = json_object_get_object_member(lens_cfg, "rtsp");
            if (rtsp) {
                GList *tiers = json_object_get_members(rtsp);
                for (GList *t = tiers; t; t = t->next) {
                    const char *tier = (const char *)t->data;
                    if (!tier) continue;
                    json_builder_set_member_name(b, tier);
                    json_builder_add_value(b, json_node_copy(json_object_get_member(rtsp, tier)));
                    found_streams = TRUE;
                }
                g_list_free(tiers);
            }
        } 
        
        /* Also scan for direct tier members (main, sub, third) to support flat configs */
        const char *standard_tiers[] = {"main", "sub", "third", "recording", NULL};
        for (int i = 0; standard_tiers[i]; i++) {
            if (json_object_has_member(lens_cfg, standard_tiers[i])) {
                json_builder_set_member_name(b, standard_tiers[i]);
                json_builder_add_value(b, json_node_copy(json_object_get_member(lens_cfg, standard_tiers[i])));
                found_streams = TRUE;
            }
        }

        if (found_streams) {
            json_builder_end_object(b);
            JsonGenerator *gen = json_generator_new();
            json_generator_set_root(gen, json_builder_get_root(b));
            char *params_json = json_generator_to_data(gen, NULL);
            
            g_info("[CONFIG] Autostart: Re-applying stored pipeline configuration for %s", lid);
            g_mutex_lock(&e->config_mutex);
            char *result = engine_configure_lens(e, params_json);
            g_mutex_unlock(&e->config_mutex);
            
            /* result could be NULL if engine_configure_lens has internal error */
            if (result) {
                g_debug("[CONFIG] Apply result: %s", result);
                g_free(result);
            }
            g_free(params_json);
            g_object_unref(gen);
        } else {
            json_builder_end_object(b);
        }
        g_object_unref(b);
    }
    g_list_free(lens_ids);
    g_object_unref(parser);
}
