/*
 * main.c — Unified Engine Entry Point (COMPLETE)
 * =================================================
 * Port of: unified_engine.py — RealUnifiedEngine class (lines 1366–2198)
 *
 * Process 1 of the architecture:
 *   - GStreamer init + CapturePipeline per lens
 *   - RTSP server with LazyRTSPFactory per stream tier
 *   - D-Bus service (com.camera.UnifiedEngine)
 *   - ZMQ publisher for alerts
 *   - DefaultConfigManager (auto-start from JSON)
 *   - System stats logging (15s periodic)
 *   - RTSP client tracking
 *   - Dynamic stream parameter updates (fps/resolution/bitrate)
 *   - GMainLoop
 */

#define _GNU_SOURCE
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../common/config.h"
#include "../common/event_broker.h"
#include "api/dbus_handlers.h"
#include "engine_types.h"
#include "gst_blocks/encoder_builder.h"
#include "subsystems/sys_capture.h"
#include "subsystems/sys_config.h"
#include "subsystems/sys_hls.h"
#include "subsystems/sys_ntp.h"
#include "subsystems/sys_recording.h"
#include "subsystems/sys_rtsp.h"
#include "subsystems/sys_stream.h"

static inline void _compute_effective_cairo_enabled(const char *lens_id,
                                                    const char *tier_name,
                                                    gboolean overlay_requested,
                                                    gboolean *cairo_active_out,
                                                    gboolean ntp_overlay_on) {
  (void)lens_id;
  (void)ntp_overlay_on;

  /* Determine if cairooverlay should actually be built into the branch.
   * Third tier always uses a plain software path — no overlay. */
  *cairo_active_out = overlay_requested;

  if (g_strcmp0(tier_name, "third") == 0) {
    *cairo_active_out = FALSE;
  }
}

static inline void
_warn_overlay_toggle_limitations(UnifiedEngine *engine, const char *lens_id,
                                 const char *tier_name, gboolean overlay_on,
                                 CapturePipeline *capture_pipe) {
  (void)engine;
  (void)capture_pipe;

  g_debug("[OVERLAY] toggle_overlay '%s/%s': "
          "Setting overlay_enabled=%s on live cairooverlay QData.  "
          "NOTE: The cairooverlay element REMAINS in the pipeline and its "
          "draw callback STILL fires every frame — drawing is just skipped.  "
          "For zero-CPU: rebuild branch with cairo_enabled=FALSE after all "
          "clients disconnect.",
          lens_id, tier_name ? tier_name : "all",
          overlay_on ? "TRUE" : "FALSE");

  if (!overlay_on) {
    LensInfo *lens_info = g_hash_table_lookup(engine->lenses, lens_id);
    if (lens_info && lens_info->cairo_capable) {
      g_warning("[OVERLAY] toggle_overlay '%s/%s': overlay disabled but "
                "LensInfo.cairo_capable=TRUE.  "
                "When clients reconnect, the branch will be rebuilt WITH "
                "cairooverlay "
                "(because cairo_capable is not updated by toggle_overlay).  "
                "To persist the disable: call ConfigureLens with cairo=false.",
                lens_id, tier_name ? tier_name : "all");
    }
  }
}

static inline void _warn_ntp_toggle_limitations(const char *lens_id,
                                                const char *tier_name,
                                                gboolean ntp_on,
                                                int overlays_toggled) {
  if (!ntp_on && overlays_toggled > 0) {
    g_warning(
        "[NTP-OVERLAY] toggle_ntp: set silent=TRUE on %d clockoverlay(s) for "
        "'%s/%s'.  "
        "IMPORTANT: clockoverlay with silent=TRUE STILL PROCESSES every frame "
        "(full GLib time render path runs, just compositing is skipped).  "
        "CPU cost per frame ≈ same as enabled.  "
        "To eliminate overhead: rebuild branch with ntp_overlay=FALSE "
        "via ConfigureLens or UpdateStreamParams.",
        overlays_toggled, lens_id, tier_name ? tier_name : "all");
  }

  if (overlays_toggled == 0) {
    g_debug(
        "[NTP-OVERLAY] toggle_ntp '%s/%s': no live clockoverlay found.  "
        "BranchConfig.ntp_overlay updated — applies on next client connect.",
        lens_id, tier_name ? tier_name : "all");
  }
}

static inline void _audit_overlay_pipeline_elements(UnifiedEngine *engine) {
  g_info("[STATS] ══ Overlay & Pipeline Element Audit ══");

  GHashTableIter iter;
  gpointer device_node_key, pipeline_val;
  g_hash_table_iter_init(&iter, engine->captures);
  while (g_hash_table_iter_next(&iter, &device_node_key, &pipeline_val)) {
    CapturePipeline *capture_pipe = (CapturePipeline *)pipeline_val;
    const char *device_path = (const char *)device_node_key;

    g_info("[STATS] Capture '%s':  cairo_capable=%s  overlay_enabled=%s  "
           "ai_coords=%d  active_branches=%u",
           device_path, capture_pipe->ai.cairo_capable ? "TRUE" : "FALSE",
           capture_pipe->ai.overlay_enabled ? "TRUE" : "FALSE",
           capture_pipe->ai.num_coords,
           g_hash_table_size(capture_pipe->dynamic_branches));

    GHashTableIter br_iter;
    gpointer branch_node_key, branch_node_val;
    g_mutex_lock(&capture_pipe->branch_mutex);
    g_hash_table_iter_init(&br_iter, capture_pipe->dynamic_branches);
    while (
        g_hash_table_iter_next(&br_iter, &branch_node_key, &branch_node_val)) {
      const char *branch_name = (const char *)branch_node_key;
      DynamicBranch *active_branch = (DynamicBranch *)branch_node_val;
      if (!active_branch->elements)
        continue;

      for (int i = 0; i < active_branch->num_elements; i++) {
        if (!active_branch->elements[i])
          continue;
        GstElementFactory *f =
            gst_element_get_factory(active_branch->elements[i]);
        if (!f)
          continue;
        const char *fname = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f));

        if (g_strcmp0(fname, "cairooverlay") == 0) {
          gboolean oe = GPOINTER_TO_INT(g_object_get_data(
              G_OBJECT(active_branch->elements[i]), "overlay_enabled"));
          int current_width = GPOINTER_TO_INT(g_object_get_data(
              G_OBJECT(active_branch->elements[i]), "branch_width"));
          int current_height = GPOINTER_TO_INT(g_object_get_data(
              G_OBJECT(active_branch->elements[i]), "branch_height"));
          g_info("[STATS]   branch='%s'  cairooverlay: overlay_enabled=%s  "
                 "branch_res=%dx%d  "
                 "(NOTE: draw callback fires every frame regardless of enabled "
                 "flag)",
                 branch_name, oe ? "TRUE" : "FALSE", current_width,
                 current_height);
        }

        if (g_strcmp0(fname, "clockoverlay") == 0) {
          gboolean silent = FALSE;
          g_object_get(active_branch->elements[i], "silent", &silent, NULL);
          g_info("[STATS]   branch='%s'  clockoverlay: silent=%s  "
                 "(NOTE: silent=TRUE still runs full render path each frame)",
                 branch_name, silent ? "TRUE" : "FALSE");
        }
      }
    }
    g_mutex_unlock(&capture_pipe->branch_mutex);
  }
}

static UnifiedEngine *g_engine = NULL;

/* ── Lens helpers (port of _get_capture, is_overlay_enabled) ── */

static CapturePipeline *_get_capture(UnifiedEngine *engine, const char *lens) {
  LensInfo *lens_info = g_hash_table_lookup(engine->lenses, lens);
  if (!lens_info)
    return NULL;
  return g_hash_table_lookup(engine->captures, lens_info->device);
}

static volatile gint g_app_log_level =
    G_LOG_LEVEL_INFO; // Fixed scope for bitbake
static volatile gboolean g_app_only_mode =
    FALSE; // APP mode: filter out GStreamer noise

/* ── [FIX-6] Output path allowlist validator ──────────────────────────────
 * Prevents the D-Bus caller from pointing recordings (or HLS segments) at
 * arbitrary filesystem locations such as /etc, /boot, or /.
 *
 * Rules:
 *   1. Path must not be NULL or empty.
 *   2. Path must not contain ".." (directory traversal).
 *   3. Path must start with one of the SAFE_PREFIXES below.
 *
 * The check is intentionally strict (allowlist, not denylist): any path that
 * does not positively match is rejected.  Adjust SAFE_PREFIXES to match the
 * deployment's storage layout. */
static gboolean _is_safe_output_path(const char *path) {
  if (!path || path[0] == '\0')
    return FALSE;
  if (strstr(path, ".."))
    return FALSE;

  static const char *const SAFE_PREFIXES[] = {
      DEFAULT_DATA_DIR, /* /data  — primary storage */
      "/tmp/smartip",   /* unit-test / CI convenience path */
      NULL};
  for (int i = 0; SAFE_PREFIXES[i]; i++) {
    if (g_str_has_prefix(path, SAFE_PREFIXES[i]))
      return TRUE;
  }
  g_warning("[SEC] Rejected unsafe output_dir: '%s' "
            "(must be under %s)",
            path, DEFAULT_DATA_DIR);
  return FALSE;
}

/* [CLEANUP]: Local config wrappers removed. Relay to sys_config.c */

static void _save_user_config_unlocked(UnifiedEngine *engine);
char *engine_update_stream(UnifiedEngine *engine, const char *lens,
                           const char *tier, const char *json);
gboolean stream_manager_is_active(StreamManager *mgr, const char *lens,
                                  const char *tier);
void stream_manager_force_teardown(StreamManager *mgr, const char *lens,
                                   const char *tier);

/* ── Global API to trigger a config sync to disk (non-locking version for
 * scheduler) ── */
void engine_persist_state(UnifiedEngine *engine) {
  _save_user_config_unlocked(engine);
}

/* ── Public API (locking version for D-Bus/API) ── */
void engine_sync_config(UnifiedEngine *engine) {
  g_mutex_lock(&engine->config_mutex);
  _save_user_config_unlocked(engine);
  g_mutex_unlock(&engine->config_mutex);
}

static void _save_user_config_unlocked(UnifiedEngine *engine) {
  if (engine->config_path[0] == '\0')
    return;

  gchar *contents = NULL;
  gsize len;
  if (!g_file_get_contents(engine->config_path, &contents, &len, NULL)) {
    if (!g_file_get_contents(DEFAULT_CONFIG_PATH, &contents, &len, NULL)) {
      contents = g_strdup("{}");
      len = 2;
    }
  }

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, len, NULL)) {
    g_free(contents);
    g_object_unref(parser);
    return;
  }
  g_free(contents);

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_warning("[CONFIG] _save_user_config: Root node missing or invalid, "
              "skipping save.");
    g_object_unref(parser);
    return;
  }
  JsonObject *root = json_node_get_object(root_node);
  JsonObject *overrides = NULL;

  if (json_object_has_member(root, "user_overrides")) {
    overrides = json_object_get_object_member(root, "user_overrides");
  }
  if (!overrides) {
    overrides = json_object_new();
    json_object_set_object_member(root, "user_overrides", overrides);
  }

  GHashTableIter lu_iter;
  gpointer lens_key, lens_val;
  g_hash_table_iter_init(&lu_iter, engine->lenses);
  while (g_hash_table_iter_next(&lu_iter, &lens_key, &lens_val)) {
    const char *lens_id = (const char *)lens_key;
    LensInfo *lens_info = (LensInfo *)lens_val;

    JsonObject *lens_obj = NULL;
    if (json_object_has_member(overrides, lens_id)) {
      lens_obj = json_object_get_object_member(overrides, lens_id);
    }
    if (!lens_obj) {
      lens_obj = json_object_new();
      json_object_set_object_member(overrides, lens_id, lens_obj);
    }

    /* [FIX-1] Persist BOTH the hardware capability flag ("cairo") AND the
     * user-visible software render state ("overlay").  Previously only
     * cairo_capable was written, so a user who had disabled the overlay via
     * ToggleOverlay or ConfigureLens would find it silently re-enabled on
     * every reboot because overlay_enabled was never saved to disk.
     * sys_config.c already forwards both keys into engine_configure_lens,
     * and engine_configure_lens reads them at the cairo/overlay branches, so
     * the restore path is complete once the save side is correct. */
    json_object_set_boolean_member(lens_obj, "cairo", lens_info->cairo_capable);
    json_object_set_boolean_member(lens_obj, "overlay",
                                   lens_info->overlay_enabled);

    /* Persist AI thresholds */
    JsonObject *ai_obj = json_object_new();
    json_object_set_double_member(ai_obj, "ai_threshold",
                                  lens_info->ai_threshold);
    json_object_set_double_member(ai_obj, "snapshot_threshold",
                                  lens_info->snapshot_threshold);
    json_object_set_object_member(lens_obj, "ai", ai_obj);
    json_object_set_boolean_member(lens_obj, "overlay",
                                   lens_info->overlay_enabled);

    /* ───── SYNC BRANCHES ───── */
    GHashTableIter br_config_iter;
    gpointer branch_node_key, branch_node_val;
    g_hash_table_iter_init(&br_config_iter, engine->branch_configs);
    while (g_hash_table_iter_next(&br_config_iter, &branch_node_key,
                                  &branch_node_val)) {
      const char *branch_key = (const char *)branch_node_key;
      BranchConfig *branch_config_ptr = (BranchConfig *)branch_node_val;
      if (!g_str_has_prefix(branch_key, lens_id))
        continue;

      const char *tier_name = strrchr(branch_key, '/');
      if (tier_name)
        tier_name++;
      else
        tier_name = branch_key;

      JsonObject *tier_obj = json_object_new();
      json_object_set_string_member(tier_obj, "resolution",
                                    branch_config_ptr->resolution);
      json_object_set_int_member(tier_obj, "fps", branch_config_ptr->fps);
      json_object_set_string_member(tier_obj, "encoder",
                                    branch_config_ptr->codec);
      json_object_set_boolean_member(tier_obj, "overlay",
                                     branch_config_ptr->overlay_enabled);
      json_object_set_boolean_member(tier_obj, "ntp_overlay",
                                     branch_config_ptr->ntp_overlay);
      json_object_set_object_member(lens_obj, tier_name, tier_obj);
    }

    /* ───── SYNC RECORDING DEFAULTS ───── */
    if (lens_info->rec_defaults) {
      JsonObject *rec_node = json_object_new();
      json_object_set_string_member(rec_node, "resolution",
                                    lens_info->rec_defaults->resolution);
      json_object_set_int_member(rec_node, "fps", lens_info->rec_defaults->fps);
      json_object_set_string_member(rec_node, "encoder",
                                    lens_info->rec_defaults->codec);
      json_object_set_int_member(rec_node, "bitrate",
                                 lens_info->rec_defaults->bitrate);
      json_object_set_object_member(lens_obj, "recording", rec_node);
    }
  }

  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, json_parser_get_root(parser));
  char *new_json = json_generator_to_data(gen, NULL);
  if (new_json) {
    /* [FIX-2] Resilient atomic save with backup rotation.
     *
     * Why this is safe:
     *   g_file_set_contents() writes to a temp sibling file then calls
     *   rename(2), which is atomic on POSIX: a concurrent reader always sees
     *   either the complete old file or the complete new file, never a partial
     *   write.  A power failure during the write leaves the original intact.
     *
     * Why we also rotate to .bak:
     *   On some embedded eMMC configurations the filesystem metadata (directory
     *   entry) is flushed before the data blocks, so after a hard power-off the
     *   new file can appear to exist with zero or garbage content even though
     *   rename(2) succeeded.  _try_load_config's JSON validator catches this
     *   and rejects the file, but without a .bak there is no intermediate
     *   fallback between user_config.json and manufacturer defaults, causing a
     *   total config wipe ("soft-brick").
     *
     * Rotation sequence (all POSIX-atomic steps):
     *   1. rename(user_config.json → user_config.json.bak)  — preserves last
     * good
     *   2. g_file_set_contents(user_config.json, new_json)  — atomic write
     *   If step 2 fails, step 1 has already moved the good copy to .bak and
     *   sys_config.c Tier-2.5 will find it on next boot. */
    GError *save_err = NULL;
    char bak_path[300];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", engine->config_path);

    if (g_file_test(engine->config_path, G_FILE_TEST_EXISTS)) {
      /* Ignore rename errors — best-effort; if it fails the old .bak (if any)
       * remains, which is still better than nothing. */
      if (g_rename(engine->config_path, bak_path) != 0) {
        g_debug("[CONFIG] .bak rotation skipped (rename errno=%d) — continuing "
                "save",
                errno);
      }
    }

    if (!g_file_set_contents(engine->config_path, new_json, -1, &save_err)) {
      g_warning("[CONFIG] SAVE FAILED for '%s': %s  "
                "(last known-good config preserved at '%s')",
                engine->config_path,
                save_err ? save_err->message : "unknown error", bak_path);
      if (save_err)
        g_error_free(save_err);
    } else {
      g_debug("[CONFIG] Saved atomically to '%s'  (backup: '%s')",
              engine->config_path, bak_path);
    }
    g_free(new_json);
  }
  g_object_unref(gen);
  g_object_unref(parser);
}

static void _save_user_config(UnifiedEngine *engine) {
  g_mutex_lock(&engine->config_mutex);
  _save_user_config_unlocked(engine);
  g_mutex_unlock(&engine->config_mutex);
}

/* ══════════════════════════════════════════════════════════════
 *  D-Bus Method Implementations
 * ══════════════════════════════════════════════════════════════ */

/* ── ConfigureLens ── */
char *engine_configure_lens(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"Empty or malformed JSON root\"}");
  }
  JsonObject *obj = json_node_get_object(root_node);
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");

  /* Ensure lens lens_info exists, or create default */
  LensInfo *lens_info = g_hash_table_lookup(engine->lenses, lens);
  if (!lens_info) {
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Unknown lens: %s\"}", lens);
  }

  gboolean previous_cairo_capable = lens_info->cairo_capable;

  if (!lens_info->ai_threshold)
    lens_info->ai_threshold = 0.4;
  if (!lens_info->snapshot_threshold)
    lens_info->snapshot_threshold = 0.4;

  if (json_object_has_member(obj, "ai")) {
    JsonObject *ai_obj = json_object_get_object_member(obj, "ai");
    if (json_object_has_member(ai_obj, "ai_threshold"))
      lens_info->ai_threshold =
          json_object_get_double_member(ai_obj, "ai_threshold");
    if (json_object_has_member(ai_obj, "snapshot_threshold"))
      lens_info->snapshot_threshold =
          json_object_get_double_member(ai_obj, "snapshot_threshold");
  }

  /* Handle cairo_capable (build-time hardware capability flag).
   * Only the dedicated "cairo" key controls this.  If the user sends
   * "overlay": false they mean "hide the visual overlay" — they do NOT mean
   * "remove the cairooverlay element from the hardware pipeline".  Conflating
   * the two caused a build-time capability change on every overlay toggle. */
  if (json_object_has_member(obj, "cairo")) {
    lens_info->cairo_capable = json_object_get_boolean_member(obj, "cairo");
  }
  /* else: keep the stored_cfg hardware capability unchanged */

  /* ───── GLOBAL STATE PROPAGATION ─────
   * If the global hardware capability changed (engine.g. Cairo disabled for the
   * lens), we must check ALL stored_cfg branches and force a rebuild on those
   * that are now out of sync, even if they aren't in the current JSON request.
   */
  if (previous_cairo_capable != lens_info->cairo_capable) {
    g_info("[CFG] Global cairo_capable changed (%s -> %s). Auditing all "
           "branches for %s",
           previous_cairo_capable ? "TRUE" : "FALSE",
           lens_info->cairo_capable ? "TRUE" : "FALSE", lens);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, engine->branch_configs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      const char *branch_key = (const char *)key;
      if (g_str_has_prefix(branch_key, lens)) {
        /* If this branch is NOT explicitly in the current JSON request object,
         * we check if it needs a force rebuild due to global capability change.
         */
        const char *slash = strchr(branch_key, '/');
        if (slash) {
          const char *tier = slash + 1;
          if (!json_object_has_member(obj, tier)) {
            g_info("[CFG] Global capability changed: Force-tearing down "
                   "out-of-sync branch '%s'",
                   branch_key);
            stream_manager_force_teardown(engine->stream_mgr, lens, tier);
            /* We don't remove from branch_configs here; the normal loop below
             * or the next client connection will handle it correctly using the
             * updated lens_info->cairo_capable flag. */
          }
        }
      }
    }
  }

  /* Handle user_overlay_on (user-visible software render tier_state).
   * BUG FIX [FIX-4]: The previous code reset user_overlay_on = cairo_capable
   * whenever "overlay" was absent from the JSON.  This silently re-enabled the
   * visual overlay every time the user called ConfigureLens for an unrelated
   * change (engine.g. FPS or resolution), discarding a previously-set disable.
   * Fix: only update user_overlay_on when the key is explicitly present. */
  if (json_object_has_member(obj, "overlay")) {
    lens_info->overlay_enabled = json_object_get_boolean_member(obj, "overlay");
  }
  /* else: preserve the stored_cfg user preference unchanged */

  /* Ensure capture running */
  const char *device = lens_info->device;
  if (!g_hash_table_contains(engine->captures, device)) {
    CapturePipeline *capture_pipe = capture_pipeline_new(
        device, engine->default_caps, lens_info->cairo_capable);
    capture_pipeline_start(capture_pipe);
    g_hash_table_insert(engine->captures, g_strdup(device), capture_pipe);
  }
  CapturePipeline *capture_pipe = g_hash_table_lookup(engine->captures, device);
  /* Propagate the new user-defined capability boolean to the main capture
   * pipeline so that [STATS] and periodic logs correctly show the overlay as
   * deactivated/disabled. */
  capture_pipe->ai.cairo_capable = lens_info->cairo_capable;

  /* Process stream tiers: main, sub, third */
  JsonBuilder *response_builder = json_builder_new();
  json_builder_begin_object(response_builder);
  json_builder_set_member_name(response_builder, "status");
  json_builder_add_string_value(response_builder, "configured");
  json_builder_set_member_name(response_builder, "lens");
  json_builder_add_string_value(response_builder, lens);
  json_builder_set_member_name(response_builder, "branches");
  json_builder_begin_object(response_builder);

  /* ── Iterate through requested tiers using GList (JSON-GLib standard) ── */
  GList *members = json_object_get_members(obj);
  for (GList *member_node = members; member_node != NULL;
       member_node = member_node->next) {
    const char *tier = (const char *)member_node->data;
    JsonNode *tier_node = json_object_get_member(obj, tier);
    if (!tier_node || !JSON_NODE_HOLDS_OBJECT(tier_node))
      continue;

    JsonObject *tier_obj = json_node_get_object(tier_node);
    const char *tier_state = "created";

    /* Handle recording defaults */
    if (g_strcmp0(tier, "recording") == 0) {
      if (!lens_info->rec_defaults)
        lens_info->rec_defaults = g_new0(BranchConfig, 1);

      const char *res_str =
          json_object_has_member(tier_obj, "resolution")
              ? json_object_get_string_member(tier_obj, "resolution")
              : "1920x1080";
      g_strlcpy(lens_info->rec_defaults->resolution, res_str,
                sizeof(lens_info->rec_defaults->resolution));
      sscanf(res_str, "%dx%d", &lens_info->rec_defaults->width,
             &lens_info->rec_defaults->height);

      lens_info->rec_defaults->fps =
          json_object_has_member(tier_obj, "fps")
              ? (int)json_object_get_int_member(tier_obj, "fps")
              : 25;

      g_strlcpy(lens_info->rec_defaults->codec,
                json_object_has_member(tier_obj, "encoder")
                    ? json_object_get_string_member(tier_obj, "encoder")
                    : "h264",
                sizeof(lens_info->rec_defaults->codec));

      lens_info->rec_defaults->bitrate =
          json_object_has_member(tier_obj, "bitrate")
              ? (int)json_object_get_int_member(tier_obj, "bitrate")
              : 4000000;

      tier_state = "configured_defaults";
    }

    if (g_strcmp0(tier, "lens") == 0 || g_strcmp0(tier, "cairo") == 0 ||
        g_strcmp0(tier, "overlay") == 0 || g_strcmp0(tier, "recording") == 0)
      continue;

    char branch_key[128], mount_path[128];
    snprintf(branch_key, sizeof(branch_key), "%s/%s", lens, tier);
    snprintf(mount_path, sizeof(mount_path), "/%s/%s", lens, tier);

    BranchConfig *stored_cfg =
        g_hash_table_lookup(engine->branch_configs, branch_key);

    /* ── FIELD COALESCING: Use stored_cfg values if JSON fields are missing ──
     */
    /* [STRICT-THIRD] Force 480p @ 3 FPS for third tier, ignore JSON/Existing */
    const char *resolution_str =
        (g_strcmp0(tier, "third") == 0)
            ? "640x480"
            : (json_object_has_member(tier_obj, "resolution")
                   ? json_object_get_string_member(tier_obj, "resolution")
                   : (stored_cfg ? stored_cfg->resolution : "1920x1080"));

    int target_fps =
        (g_strcmp0(tier, "third") == 0)
            ? 3
            : (json_object_has_member(tier_obj, "fps")
                   ? (int)json_object_get_int_member(tier_obj, "fps")
                   : (stored_cfg ? stored_cfg->fps : 25));

    const char *codec_name =
        (g_strcmp0(tier, "third") == 0)
            ? "h264"  /* was "mjpeg" — switched to x264enc software H.264 to reduce CPU load */
            : (json_object_has_member(tier_obj, "encoder")
                   ? json_object_get_string_member(tier_obj, "encoder")
                   : (stored_cfg ? stored_cfg->codec : "h264"));

    gboolean user_overlay_on =
        json_object_has_member(tier_obj, "overlay")
            ? json_object_get_boolean_member(tier_obj, "overlay")
            : (stored_cfg ? stored_cfg->overlay_enabled : TRUE);

    gboolean ntp_on =
        json_object_has_member(tier_obj, "ntp_overlay")
            ? json_object_get_boolean_member(tier_obj, "ntp_overlay")
            : (json_object_has_member(tier_obj, "ntp_on")
                   ? json_object_get_boolean_member(tier_obj, "ntp_on")
                   : (stored_cfg ? stored_cfg->ntp_overlay : FALSE));

    /* ── SCENARIO CHECK ── */
    gboolean stream_is_live =
        stream_manager_is_active(engine->stream_mgr, lens, tier);

    if (stored_cfg) {
      /* Compute the EFFECTIVE cairo state for the old and new configurations.
       * These fold in lens_info->cairo_capable so that disabling cairo at the
       * lens level is correctly detected as a structural change even when the
       * raw user_overlay_on flag is the same. */
      gboolean was_cairo_active;
      _compute_effective_cairo_enabled(
          lens, tier, previous_cairo_capable && stored_cfg->overlay_enabled,
          &was_cairo_active, stored_cfg->ntp_overlay);

      gboolean will_cairo_be_active;
      _compute_effective_cairo_enabled(
          lens, tier, lens_info->cairo_capable && user_overlay_on,
          &will_cairo_be_active, ntp_on);

      /* A structural rebuild is required when codec, effective cairo state, or
       * NTP overlay changes — these alter which GStreamer elements are in the
       * branch and cannot be applied dynamically.
       * Resolution and FPS are dynamic-update safe (videorate / capsfilter). */
      gboolean codec_unchanged =
          (g_strcmp0(stored_cfg->codec, codec_name) == 0);

      /* BUG FIX [FIX-1]: was_cairo_active / will_cairo_be_active are used here
       * instead of the raw user_overlay_on flags.  The previous code compared
       * stored_cfg->user_overlay_on == user_overlay_on, which ignored changes
       * to lens_info->cairo_capable.  Disabling cairo at the lens level would
       * leave user_overlay_on unchanged, so overlay_match returned TRUE and the
       * cairooverlay element was never stripped from the branch. */
      gboolean pipeline_structure_matches =
          (was_cairo_active == will_cairo_be_active &&
           stored_cfg->ntp_overlay == ntp_on);

      if (codec_unchanged && pipeline_structure_matches) {
        /* ───── DYNAMIC UPDATE ───── */
        stored_cfg->fps = target_fps;
        sscanf(resolution_str, "%dx%d", &stored_cfg->width,
               &stored_cfg->height);
        g_strlcpy(stored_cfg->resolution, resolution_str,
                  sizeof(stored_cfg->resolution));
        stored_cfg->overlay_enabled = user_overlay_on;
        stored_cfg->ntp_overlay = ntp_on;

        if (stream_is_live) {
          g_info("[CFG] Active path: Propagating dynamic params to '%s/%s'",
                 lens, tier);
          char *dynamic_update_json = g_strdup_printf(
              "{\"resolution\":\"%s\",\"fps\":%d,\"overlay\":%"
              "s,\"ntp_overlay\":%s}",
              resolution_str, target_fps, user_overlay_on ? "true" : "false",
              ntp_on ? "true" : "false");
          engine_update_stream(engine, lens, tier, dynamic_update_json);
          g_free(dynamic_update_json);
        } else {
          g_info("[CFG] Idle path: Saved dynamic config for '%s/%s' (will "
                 "apply on next connect)",
                 lens, tier);
        }
        tier_state = "updated";
      } else {
        /* ───── STATIC REBUILD ─────
         * Codec, overlay pipeline composition, or NTP changed.
         * Remove the RTSP rtsp_factory, tear down the live GStreamer branch
         * immediately (clients must reconnect), clear StreamManager state,
         * and let the !stored_cfg block below re-create everything with the
         * updated parameters. */
        g_info("[CFG] Static change for '%s/%s' (Codec/Overlay Build) -> "
               "REBUILDING  stream_is_live=%s",
               lens, tier, stream_is_live ? "YES" : "NO");

        GstRTSPMountPoints *rtsp_mounts =
            gst_rtsp_server_get_mount_points(engine->rtsp_server);
        gst_rtsp_mount_points_remove_factory(rtsp_mounts, mount_path);
        g_object_unref(rtsp_mounts);

        /* Remove the live GStreamer branch immediately so the new pipeline
         * is ready for the next client connect, without waiting for all
         * current clients to voluntarily disconnect. */
        if (stream_is_live) {
          const char *live_branch_name = stream_manager_get_active_branch_name(
              engine->stream_mgr, lens, tier);
          if (live_branch_name && capture_pipe) {
            g_info("[CFG]   Force-removing live branch '%s' from pipeline",
                   live_branch_name);
            capture_pipeline_remove_branch(capture_pipe, live_branch_name);
          }
        }

        stream_manager_force_teardown(engine->stream_mgr, lens, tier);

        g_hash_table_remove(engine->branch_configs, branch_key);
        stored_cfg = NULL;
        tier_state = "rebuilt";
      }
    }

    if (!stored_cfg) {
      BranchConfig *new_branch_cfg =
          branch_config_new(resolution_str, target_fps, codec_name);
      new_branch_cfg->ntp_overlay = ntp_on;
      new_branch_cfg->overlay_enabled = user_overlay_on;

      /* Honor bitrate/mode from JSON at bootup, falling back to Lens defaults then Global defaults */
      new_branch_cfg->bitrate =
          json_object_has_member(tier_obj, "bitrate")
              ? (int)json_object_get_int_member(tier_obj, "bitrate")
              : (lens_info->rec_defaults ? lens_info->rec_defaults->bitrate
                                         : DEFAULT_BRANCH_BITRATE);
      g_strlcpy(new_branch_cfg->bitrate_mode,
                json_object_has_member(tier_obj, "bitrate_mode")
                    ? json_object_get_string_member(tier_obj, "bitrate_mode")
                    : (lens_info->rec_defaults
                           ? lens_info->rec_defaults->bitrate_mode
                           : DEFAULT_BRANCH_BITRATE_MODE),
                sizeof(new_branch_cfg->bitrate_mode));

      g_hash_table_insert(engine->branch_configs, g_strdup(branch_key),
                          new_branch_cfg);

      /* Exclude cairo from build if hardware disabled OR user disabled */
      gboolean build_cairo;
      _compute_effective_cairo_enabled(
          lens, tier, lens_info->cairo_capable && user_overlay_on, &build_cairo,
          ntp_on);

      /* Mount RTSP rtsp_factory */
      GstRTSPMountPoints *rtsp_mounts =
          gst_rtsp_server_get_mount_points(engine->rtsp_server);
      GstRTSPMediaFactory *rtsp_factory = lazy_rtsp_factory_create(
          engine, capture_pipe, engine->stream_mgr, lens, tier, new_branch_cfg,
          mount_path, build_cairo);
      gst_rtsp_mount_points_add_factory(rtsp_mounts, mount_path, rtsp_factory);
      g_object_unref(rtsp_mounts);
    }

    g_info("Configured: rtsp://0.0.0.0:%d%s (%s %dfps %s) [%s]",
           engine->rtsp_port, mount_path, resolution_str, target_fps,
           codec_name, tier_state);

    /* Add to result */
    json_builder_set_member_name(response_builder, tier);
    json_builder_begin_object(response_builder);
    json_builder_set_member_name(response_builder, "mount");
    json_builder_add_string_value(response_builder, mount_path);
    json_builder_set_member_name(response_builder, "resolution");
    json_builder_add_string_value(response_builder, resolution_str);
    json_builder_set_member_name(response_builder, "fps");
    json_builder_add_int_value(response_builder, target_fps);
    json_builder_set_member_name(response_builder, "encoder");
    json_builder_add_string_value(response_builder, codec_name);
    json_builder_set_member_name(response_builder, "state");
    json_builder_add_string_value(response_builder, tier_state);
    json_builder_end_object(response_builder);
  }
  g_list_free(members);

  /* ── FORCED AUTO-START: Ensure 'third' branch is always mounted at boot ── */
  char third_key[128];
  snprintf(third_key, sizeof(third_key), "%s/third", lens);
  if (!g_hash_table_contains(engine->branch_configs, third_key)) {
    g_info("[CFG]   Auto-mounting mandatory 'third' branch for %s", lens);

    BranchConfig *new_branch_cfg = branch_config_new("640x480", 3, "h264");  /* was "mjpeg" */
    new_branch_cfg->ntp_overlay = FALSE;
    new_branch_cfg->overlay_enabled = FALSE;
    g_hash_table_insert(engine->branch_configs, g_strdup(third_key),
                        new_branch_cfg);

    char mount[128];
    snprintf(mount, sizeof(mount), "/%s/third", lens);

    GstRTSPMountPoints *rtsp_mounts =
        gst_rtsp_server_get_mount_points(engine->rtsp_server);
    GstRTSPMediaFactory *rtsp_factory =
        lazy_rtsp_factory_create(engine, capture_pipe, engine->stream_mgr, lens,
                                 "third", new_branch_cfg, mount, FALSE);
    gst_rtsp_mount_points_add_factory(rtsp_mounts, mount, rtsp_factory);
    g_object_unref(rtsp_mounts);

    g_info("Configured: rtsp://0.0.0.0:%d%s (640x480 3fps h264/x264enc) [auto-start]",
           engine->rtsp_port, mount);
  }

  json_builder_end_object(response_builder); /* branches */
  json_builder_end_object(response_builder); /* root */

  _save_user_config_unlocked(engine);

  JsonGenerator *result_gen = json_generator_new();
  json_generator_set_root(result_gen, json_builder_get_root(response_builder));
  char *result = json_generator_to_data(result_gen, NULL);
  g_object_unref(result_gen);
  g_object_unref(response_builder);
  g_object_unref(parser);
  return result;
}

/* ── Pad probe callback for safe dynamic resolution change ──
 *
 * This callback fires on the streaming thread once all in-flight buffers
 * have drained from the capsfilter's sink pad.  At this point it is safe
 * to change the caps because no old-resolution buffer can slip through.
 *
 * Steps:
 *   1. Retrieve the pending caps stored as QData on the pad
 *   2. Set the new caps on the capsfilter element
 *   3. Send RECONFIGURE event upstream so the scaler renegotiates
 *   4. Return GST_PAD_PROBE_REMOVE to unblock data flow
 */
/* ── Legacy Pad probe callback (Drop-1 version) ──
 *
 * This was the intermediate fix that dropped exactly one frame.
 * Preserved here for fallback/reference as requested by the user.
 */
static GstPadProbeReturn
_resolution_change_probe_legacy_drop_1(GstPad *pad, GstPadProbeInfo *info,
                                       gpointer user_data) {
  GstElement *capsfilter_elem = GST_ELEMENT(user_data);
  GstCaps *new_caps = g_object_get_data(G_OBJECT(pad), "pending_caps");
  if (!new_caps) {
    gst_pad_remove_probe(pad, info->id);
    return GST_PAD_PROBE_DROP;
  }
  gst_caps_ref(new_caps);
  g_object_set(capsfilter_elem, "caps", new_caps, NULL);
  GstPad *peer_pad = gst_pad_get_peer(pad);
  if (peer_pad) {
    gst_pad_send_event(peer_pad, gst_event_new_reconfigure());
    gst_object_unref(peer_pad);
  }
  gst_caps_unref(new_caps);
  g_object_set_data(G_OBJECT(pad), "pending_caps", NULL);
  gst_pad_remove_probe(pad, info->id);
  return GST_PAD_PROBE_DROP;
}

/* ── MODULAR STABILITY GUARDS ── */

static int _clamp_bitrate_by_fps(int fps, int requested_bps) {
  g_message("[STABILITY-TRACE] Entering clamp check: %d bps @ %d FPS",
            requested_bps, fps);
  int min_bps = 50000;    /* 50 kbps */
  int max_bps_ceiling;

  if (fps < 10) {
    /* Rule: 700,000 bits budget per frame, capped at 7 Mbps */
    max_bps_ceiling = fps * 700000;
    if (max_bps_ceiling > 7000000)
      max_bps_ceiling = 7000000;
  } else {
    /* Rule: 600,000 bits budget per frame, capped at 16 Mbps */
    max_bps_ceiling = fps * 600000;
    if (max_bps_ceiling > 16000000)
      max_bps_ceiling = 16000000;
  }

  if (requested_bps < min_bps) {
    g_info("[STABILITY] Bitrate %d bps clamped to %d bps for %d FPS. Suggested "
           "range: %d-%d bps.",
           requested_bps, min_bps, fps, min_bps, max_bps_ceiling);
    return min_bps;
  }
  if (requested_bps > max_bps_ceiling) {
    g_info("[STABILITY] Bitrate %d bps clamped to %d bps for %d FPS. Suggested "
           "range: %d-%d bps.",
           requested_bps, max_bps_ceiling, fps, min_bps, max_bps_ceiling);
    return max_bps_ceiling;
  }

  return requested_bps;
}

static int _calculate_gop_for_fps(int fps) {
  /* Rule: I-frames every 2 seconds */
  int gop = fps * 2;
  g_message("[STABILITY-TRACE] Calculated GOP: %d for %d FPS", gop, fps);
  return gop;
}

static void _force_encoder_keyframe(GstElement *enc) {
  if (!enc)
    return;
  g_message("[STABILITY] Requesting immediate IDR (Keyframe) for encoder %s",
            GST_ELEMENT_NAME(enc));

  /* Trigger standard GStreamer force-keyunit event */
  GstStructure *s =
      gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, NULL);
  GstEvent *event = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, s);
  gst_element_send_event(enc, event);
}

/* ── BULLETPROOF Pad probe callback (Wait-for-Caps version) ──
 *
 * This version is 100% stable regardless of hardware latency.
 * Logic:
 *   1. When first called (DATA_DOWNSTREAM block): apply new caps to filter
 *      and send RECONFIGURE upstream.
 *   2. For all following BUFFERS: return DROP.
 *   3. For the next CAPS event: if it matches target resolution,
 *      REMOVE the probe and return OK (letting the new video flow).
 */
static GstPadProbeReturn _resolution_change_probe_cb(GstPad *pad,
                                                     GstPadProbeInfo *info,
                                                     gpointer user_data) {
  GstElement *capsfilter_elem = GST_ELEMENT(user_data);

  /* Get the target caps we want to transition to */
  GstCaps *target_caps = g_object_get_data(G_OBJECT(pad), "pending_caps");
  if (!target_caps) {
    g_warning("[RES-PROBE] No target caps found on pad — aborting");
    gst_pad_remove_probe(pad, info->id);
    return GST_PAD_PROBE_OK;
  }

  /* ───── 1. INITIAL RECONFIGURE TRIGGER ───── */
  /* We do this as early as possible so that hardware reconfiguration starts
   * even if the very first thing we catch is a buffer. */
  if (!g_object_get_data(G_OBJECT(pad), "reconfigure_sent")) {
    g_message("[RES-PROBE] >>> Transition started: Setting caps and sending "
              "RECONFIGURE upstream");
    g_object_set(capsfilter_elem, "caps", target_caps, NULL);

    GstPad *peer_pad = gst_pad_get_peer(pad);
    if (peer_pad) {
      gst_pad_send_event(peer_pad, gst_event_new_reconfigure());
      gst_object_unref(peer_pad);
    }
    g_object_set_data(G_OBJECT(pad), "reconfigure_sent", GINT_TO_POINTER(1));
  }

  /* ───── 2. HANDLE EVENTS (CAPS) ───── */
  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = gst_pad_probe_info_get_event(info);

    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
      GstCaps *incoming_caps;
      gst_event_parse_caps(event, &incoming_caps);

      g_message("[RES-PROBE] -> Received new CAPS event from hardware");

      if (gst_caps_is_subset(incoming_caps, target_caps)) {
        g_message(
            "[RES-PROBE] ✓ CAPS match target! Removing probe and unblocking.");

        /* Cleanup state and remove probe */
        g_object_set_data(G_OBJECT(pad), "pending_caps", NULL);
        g_object_set_data(G_OBJECT(pad), "reconfigure_sent", NULL);
        gst_pad_remove_probe(pad, info->id);
        return GST_PAD_PROBE_OK;
      } else {
        g_message("[RES-PROBE] -> CAPS do not match target yet, continuing to "
                  "block...");
      }
    }

    /* For non-CAPS events, let them pass through the block */
    return GST_PAD_PROBE_OK;
  }

  /* ───── 3. HANDLE BUFFERS ───── */
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    /* All buffers captured while the probe is active (after reconfigure was
     * sent but before new CAPS arrival) are in the old resolution. Drop them.
     */
    g_message("[RES-PROBE] -> Dropping stale frame...");
    return GST_PAD_PROBE_DROP;
  }

  return GST_PAD_PROBE_OK;
}

/* ── UpdateStreamParams / _apply_dynamic_update ── */
char *engine_update_stream(UnifiedEngine *engine, const char *lens,
                           const char *branch, const char *json) {
  char branch_key[128];
  snprintf(branch_key, sizeof(branch_key), "%s/%s", lens, branch);
  BranchConfig *branch_config_ptr =
      g_hash_table_lookup(engine->branch_configs, branch_key);
  if (!branch_config_ptr) {
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Branch '%s' not configured\"}",
        branch_key);
  }

  CapturePipeline *capture_pipe = _get_capture(engine, lens);
  if (!capture_pipe) {
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"No capture for %s\"}", lens);
  }

  /* Handle direct ntp_overlay update */
  if (g_str_has_suffix(json, "ntp_overlay")) {
    /* Fast path for specialized ToggleNTP if JSON is just the toggle */
    /* But we also handle it in the standard update_params below */
  }

  JsonParser *json_parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(json_parser, json, -1, &err)) {
    char *response = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(json_parser);
    return response;
  }

  JsonNode *json_root_node = json_parser_get_root(json_parser);
  if (!json_root_node || !JSON_NODE_HOLDS_OBJECT(json_root_node)) {
    g_object_unref(json_parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"Empty or malformed JSON root\"}");
  }
  JsonObject *update_params = json_node_get_object(json_root_node);
  gboolean any_param_changed = FALSE;

  /* Find active branch elements — LOCK to prevent teardown from
   * nulling active_branch->elements while we iterate them. */
  g_mutex_lock(&capture_pipe->branch_mutex);

  /* Query StreamManager for the actual, active branch name (contains unique
   * UID). This handles the naming mismatch between Engine and Manager while
   * avoiding races. */
  const char *active_branch_name =
      stream_manager_get_active_branch_name(engine->stream_mgr, lens, branch);
  DynamicBranch *active_branch = NULL;

  if (active_branch_name) {
    active_branch =
        g_hash_table_lookup(capture_pipe->dynamic_branches, active_branch_name);
  } else {
    /* Legacy fallback for non-shared branches (if any) */
    char legacy_branch_name[64];
    snprintf(legacy_branch_name, sizeof(legacy_branch_name), "rtsp_%s_%s", lens,
             branch);
    active_branch =
        g_hash_table_lookup(capture_pipe->dynamic_branches, legacy_branch_name);
  }

  /* NEVER fall back to rec_ branches here!
     Dynamic stream updates on active MP4 recordings corrupt the muxer container
     headers. */

  if (!active_branch || !active_branch->elements) {
    g_mutex_unlock(&capture_pipe->branch_mutex);
    /* Branch idle — just save config for next connect */
    if (json_object_has_member(update_params, "fps"))
      branch_config_ptr->fps =
          (int)json_object_get_int_member(update_params, "fps");
    if (json_object_has_member(update_params, "resolution")) {
      const char *resolution_str =
          json_object_get_string_member(update_params, "resolution");
      g_strlcpy(branch_config_ptr->resolution, resolution_str,
                sizeof(branch_config_ptr->resolution));
      sscanf(resolution_str, "%dx%d", &branch_config_ptr->width,
             &branch_config_ptr->height);
    }
    if (json_object_has_member(update_params, "ntp_overlay"))
      branch_config_ptr->ntp_overlay =
          json_object_get_boolean_member(update_params, "ntp_overlay");
    if (json_object_has_member(update_params, "overlay"))
      branch_config_ptr->overlay_enabled =
          json_object_get_boolean_member(update_params, "overlay");
    _save_user_config(engine);
    g_object_unref(json_parser);
    return g_strdup("{\"status\":\"config_updated\",\"note\":\"Branch idle, "
                    "config saved for next connect\"}");
  }

  /* Apply Cairo Overlay toggle dynamically */
  if (json_object_has_member(update_params, "overlay")) {
    gboolean overlay_on =
        json_object_get_boolean_member(update_params, "overlay");
    gboolean cairo_elem_found = FALSE;
    for (int i = 0; i < active_branch->num_elements; i++) {
      const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
      if (g_str_has_prefix(name, "cairo_")) {
        g_object_set_data(G_OBJECT(active_branch->elements[i]),
                          "overlay_enabled", GINT_TO_POINTER(overlay_on));
        cairo_elem_found = TRUE;
        any_param_changed = TRUE;
        g_info("Dynamic update: %s overlay → %s", branch_key,
               overlay_on ? "ON" : "OFF");
      }
    }
    branch_config_ptr->overlay_enabled = overlay_on;
    if (!cairo_elem_found) {
      g_info("Dynamic update: %s overlay config saved (%s). "
             "Element not in pipeline — reconfigure lens to include it.",
             branch_key, overlay_on ? "ON" : "OFF");
      any_param_changed = TRUE;
    }
  }

  /* Apply NTP overlay change (clockoverlay) */
  if (json_object_has_member(update_params, "ntp_overlay")) {
    gboolean ntp_on =
        json_object_get_boolean_member(update_params, "ntp_overlay");
    gboolean clock_elem_found = FALSE;
    for (int i = 0; i < active_branch->num_elements; i++) {
      GstElementFactory *f =
          gst_element_get_factory(active_branch->elements[i]);
      if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)),
                         "clockoverlay") == 0) {
        g_object_set(active_branch->elements[i], "silent", !ntp_on, NULL);
        clock_elem_found = TRUE;
        any_param_changed = TRUE;
        g_info("Dynamic update: %s ntp_overlay → %s", branch_key,
               ntp_on ? "ON" : "OFF");
      }
    }
    /* Always save the config — if clockoverlay wasn't in the pipeline
     * (because it was excluded at build time), the change takes effect
     * on next stream connect when the branch is rebuilt. */
    branch_config_ptr->ntp_overlay = ntp_on;
    if (!clock_elem_found) {
      g_info("Dynamic update: %s ntp_overlay config saved (%s). "
             "Takes effect on next stream connect.",
             branch_key, ntp_on ? "ON" : "OFF");
      any_param_changed = TRUE;
    }
  }

  /* Apply FPS change to videorate element, fps capsfilter, and sync GOP/Bitrate stability */
  if (json_object_has_member(update_params, "fps") &&
      g_strcmp0(branch, "third") != 0) {
    int target_fps = (int)json_object_get_int_member(update_params, "fps");
    /* [STABILITY] Clamp FPS to global maximum of 30. */
    int effective_fps =
        (target_fps > 30) ? 30 : ((target_fps > 0) ? target_fps : 30);

    if (branch_config_ptr->fps != effective_fps) {
      int prev_fps = branch_config_ptr->fps;
      branch_config_ptr->fps = effective_fps;
      any_param_changed = TRUE;

      /* Recalculate stability parameters for the new FPS */
      int effective_bitrate =
          _clamp_bitrate_by_fps(effective_fps, branch_config_ptr->bitrate);
      int effective_gop = _calculate_gop_for_fps(effective_fps);

      for (int i = 0; i < active_branch->num_elements; i++) {
        GstElement *elem = active_branch->elements[i];
        const char *name = GST_ELEMENT_NAME(elem);

        if (g_str_has_prefix(name, "rate_")) {
          g_object_set(elem, "max-rate", effective_fps, NULL);
          g_info("Dynamic update: %s fps → %d (videorate)", branch_key,
                 effective_fps);
        }
        if (g_str_has_prefix(name, "fps_caps_")) {
          char fps_str[128];
          snprintf(fps_str, sizeof(fps_str), "video/x-raw,framerate=%d/1",
                   effective_fps);
          GstCaps *new_fps_caps = gst_caps_from_string(fps_str);
          g_object_set(elem, "caps", new_fps_caps, NULL);
          gst_caps_unref(new_fps_caps);
          g_info("Dynamic update: %s fps → %d (capsfilter)", branch_key,
                 effective_fps);
        }
        /* Sync encoder with new FPS-derived stability limits */
        if (g_str_has_prefix(name, "enc_")) {
          GstElementFactory *f = gst_element_get_factory(elem);
          const char *fname =
              f ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)) : "";

          if (strstr(fname, "v4l2")) {
            char ctrl[256];
            snprintf(ctrl, sizeof(ctrl),
                     "controls,video_bitrate=%d,video_gop_size=%d",
                     effective_bitrate, effective_gop);
            GstStructure *ec = gst_structure_new_from_string(ctrl);
            if (ec) {
              g_object_set(elem, "extra-controls", ec, NULL);
              gst_structure_free(ec);
              g_info("Dynamic update: %s synced encoder stability (GOP=%d, Bitrate=%d) due to FPS change",
                     branch_key, effective_gop, effective_bitrate);
            }
          }
          _force_encoder_keyframe(elem);
        }
      }
      branch_config_ptr->bitrate = effective_bitrate;
    } else {
      g_info("Dynamic update: %s FPS is already %d (skipping)", branch_key,
             effective_fps);
    }
  }

  /* Apply resolution change to capsfilter + update cairooverlay dimensions.
   *
   * [FIX-RESOLUTION-RACE] The capsfilter caps change must be synchronized
   * with the data flow.  Without blocking, an old-resolution buffer can
   * arrive at the capsfilter after new caps are set but before the upstream
   * scaler (G2D) renegotiates, causing "not negotiated" → pipeline crash.
   *
   * Fix: Install a BLOCK_DOWNSTREAM probe on the capsfilter's sink pad.
   * Inside the probe (which fires when data flow is safely blocked):
   *   1. Update the capsfilter's caps property
   *   2. Send GST_EVENT_RECONFIGURE upstream to tell the scaler to renegotiate
   *   3. Remove the probe (GST_PAD_PROBE_REMOVE) to resume data flow
   *
   * This is the standard GStreamer pattern for live caps renegotiation. */
  if (json_object_has_member(update_params, "resolution") &&
      g_strcmp0(branch, "third") != 0) {
    const char *resolution_str =
        json_object_get_string_member(update_params, "resolution");

    if (g_strcmp0(branch_config_ptr->resolution, resolution_str) == 0) {
      g_info("Dynamic update: %s resolution is already %s (skipping probe)",
             branch_key, resolution_str);
    } else {
      int width, height;
      sscanf(resolution_str, "%dx%d", &width, &height);
      for (int i = 0; i < active_branch->num_elements; i++) {
        const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
        if (g_str_has_prefix(name, "res_")) {
          GstElement *capsfilter_elem = active_branch->elements[i];

          /* Build new caps string */
          char caps_str[256];
          snprintf(caps_str, sizeof(caps_str), "video/x-raw,width=%d,height=%d",
                   width, height);
          GstCaps *new_caps = gst_caps_from_string(caps_str);

          /* Get the sink pad of the capsfilter and block data flow */
          GstPad *sink_pad = gst_element_get_static_pad(capsfilter_elem, "sink");
          if (sink_pad) {
            /* Store new caps as QData on the pad so the probe callback
             * can retrieve and apply them atomically. */
            g_object_set_data_full(G_OBJECT(sink_pad), "pending_caps", new_caps,
                                   (GDestroyNotify)gst_caps_unref);

            gst_pad_add_probe(sink_pad,
                              GST_PAD_PROBE_TYPE_BLOCK |
                                  GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
                              _resolution_change_probe_cb, capsfilter_elem,
                              NULL);
            gst_object_unref(sink_pad);
            g_info("Dynamic update: %s resolution probe installed → waiting "
                   "for block to apply %s",
                   branch_key, resolution_str);
          } else {
            /* Fallback: no sink pad (shouldn't happen) — apply directly */
            g_object_set(capsfilter_elem, "caps", new_caps, NULL);
            gst_caps_unref(new_caps);
            g_warning("Dynamic update: %s capsfilter has no sink pad, applied "
                      "caps directly (unsafe)",
                      branch_key);
          }

          g_strlcpy(branch_config_ptr->resolution, resolution_str,
                    sizeof(branch_config_ptr->resolution));
          branch_config_ptr->width = width;
          branch_config_ptr->height = height;
          any_param_changed = TRUE;
          g_info("Dynamic update: %s resolution → %s", branch_key,
                 resolution_str);
        }
        /* Also update cairooverlay branch dimensions so bounding boxes scale
         * correctly */
        if (g_str_has_prefix(name, "cairo_")) {
          g_object_set_data(G_OBJECT(active_branch->elements[i]), "branch_width",
                            GINT_TO_POINTER(width));
          g_object_set_data(G_OBJECT(active_branch->elements[i]),
                            "branch_height", GINT_TO_POINTER(height));
          g_info("Dynamic update: %s cairooverlay dimensions → %dx%d",
                 branch_key, width, height);
        }
      }
    }
  }

  /* Apply bitrate change with safety clamping and Keyframe reset */
  if (json_object_has_member(update_params, "bitrate")) {
    int requested_bps =
        (int)json_object_get_int_member(update_params, "bitrate");
    g_info("[STABILITY-TRACE] >>> API Request: Change bitrate for %s to %d bps",
           branch_key, requested_bps);

    int effective_bps =
        _clamp_bitrate_by_fps(branch_config_ptr->fps, requested_bps);

    if (branch_config_ptr->bitrate != effective_bps) {
      for (int i = 0; i < active_branch->num_elements; i++) {
        GstElement *elem = active_branch->elements[i];
        const char *name = GST_ELEMENT_NAME(elem);

        if (g_str_has_prefix(name, "enc_")) {
          GstElementFactory *elem_factory = gst_element_get_factory(elem);
          const char *fname =
              elem_factory
                  ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(elem_factory))
                  : "";
          if (strstr(fname, "v4l2")) {
            char ctrl[256];
            /* Maintain existing GOP while updating bitrate */
            int current_gop = _calculate_gop_for_fps(branch_config_ptr->fps);
            snprintf(ctrl, sizeof(ctrl),
                     "controls,video_bitrate=%d,video_gop_size=%d",
                     effective_bps, current_gop);
            GstStructure *ec = gst_structure_new_from_string(ctrl);
            if (ec) {
              g_object_set(elem, "extra-controls", ec, NULL);
              gst_structure_free(ec);
              any_param_changed = TRUE;
            }
          } else if (strstr(fname, "x264")) {
            /* x264enc expects kbps */
            g_object_set(elem, "bitrate", effective_bps / 1000, NULL);
            any_param_changed = TRUE;
          }
          _force_encoder_keyframe(elem);
          g_info("Dynamic update: %s bitrate → %d bps (requested %d)",
                 branch_key, effective_bps, requested_bps);
          break;
        }
      }
      branch_config_ptr->bitrate = effective_bps;
    } else {
      g_info("Dynamic update: %s bitrate is already %d bps (skipping)",
             branch_key, effective_bps);
    }
  }

  /* Apply bitrate mode change (VBR/CBR) to encoder */
  if (json_object_has_member(update_params, "bitrate_mode")) {
    const char *mode =
        json_object_get_string_member(update_params, "bitrate_mode");
    for (int i = 0; i < active_branch->num_elements; i++) {
      const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
      if (g_str_has_prefix(name, "enc_")) {
        GstElementFactory *elem_factory =
            gst_element_get_factory(active_branch->elements[i]);
        const char *fname =
            elem_factory
                ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(elem_factory))
                : "";
        if (strstr(fname, "v4l2")) {
          /* VPU hw encoder NXP patch: "vbr-mode" property. 1=VBR, 0=CBR */
          int vbm = (g_strcmp0(mode, "vbr") == 0) ? 1 : 0;
          if (g_object_class_find_property(
                  G_OBJECT_GET_CLASS(active_branch->elements[i]), "vbr-mode")) {
            g_object_set(active_branch->elements[i], "vbr-mode", vbm, NULL);
          } else {
            g_warning("Encoder has no 'vbr-mode' property (NXP patch not "
                      "applied). Skipping.");
          }
          any_param_changed = TRUE;
        } else if (strstr(fname, "x264")) {
          /* x264enc: pass=0 → CBR, pass=17 → quality-based VBR */
          int pass = (g_strcmp0(mode, "cbr") == 0) ? 0 : 17;
          g_object_set(active_branch->elements[i], "pass", pass, NULL);
          any_param_changed = TRUE;
        }
        g_info("Dynamic update: %s bitrate_mode → %s", branch_key, mode);
        break;
      }
    }
  }

  g_mutex_unlock(&capture_pipe->branch_mutex);

  char *response = any_param_changed
                       ? g_strdup_printf("{\"status\":\"any_param_changed\","
                                         "\"lens\":\"%s\",\"branch\":\"%s\"}",
                                         lens, branch)
                       : g_strdup("{\"status\":\"no_change\"}");

  if (any_param_changed)
    _save_user_config(engine);

  g_object_unref(json_parser);
  return response;
}

/* ── StopLens (full cascade teardown) ── */
char *engine_stop_lens(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "");
  LensInfo *lens_info = g_hash_table_lookup(engine->lenses, lens);
  if (!lens_info) {
    g_object_unref(parser);
    return g_strdup("{\"status\":\"error\",\"message\":\"Unknown lens\"}");
  }

  /* Stop recordings for this lens */
  GHashTableIter iter;
  gpointer k, v;
  GList *keys_to_remove = NULL;
  g_hash_table_iter_init(&iter, engine->recordings);
  while (g_hash_table_iter_next(&iter, &k, &v)) {
    if (g_str_has_prefix((const char *)k, lens)) {
      /* Destructor in hash table handles stopping */
      keys_to_remove = g_list_append(keys_to_remove, g_strdup((const char *)k));
    }
  }
  for (GList *l = keys_to_remove; l; l = l->next) {
    g_hash_table_remove(engine->recordings, l->data);
    g_free(l->data);
  }
  g_list_free(keys_to_remove);

  /* Remove RTSP factories for this lens */
  GstRTSPMountPoints *mounts =
      gst_rtsp_server_get_mount_points(engine->rtsp_server);
  keys_to_remove = NULL;
  g_hash_table_iter_init(&iter, engine->branch_configs);
  while (g_hash_table_iter_next(&iter, &k, &v)) {
    if (g_str_has_prefix((const char *)k, lens)) {
      char mount[64];
      snprintf(mount, sizeof(mount), "/%s", (const char *)k);
      gst_rtsp_mount_points_remove_factory(mounts, mount);
      keys_to_remove = g_list_append(keys_to_remove, g_strdup((const char *)k));
    }
  }
  g_object_unref(mounts);
  for (GList *l = keys_to_remove; l; l = l->next) {
    g_hash_table_remove(engine->branch_configs, l->data);
    g_free(l->data);
  }
  g_list_free(keys_to_remove);

  /* Stop capture pipeline */
  CapturePipeline *capture_pipe =
      g_hash_table_lookup(engine->captures, lens_info->device);
  if (capture_pipe) {
    capture_pipeline_stop(capture_pipe);
    g_hash_table_remove(engine->captures, lens_info->device);
  }

  char *resp =
      g_strdup_printf("{\"status\":\"stopped\",\"lens\":\"%s\"}", lens);
  g_object_unref(parser);
  return resp;
}

/* ── TakeSnapshot ── */
char *engine_take_snapshot(UnifiedEngine *engine, const char *lens,
                           const char *path) {
  if (!lens || !path || path[0] == '\0')
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"lens and path required\"}");

  CapturePipeline *capture_pipe = _get_capture(engine, lens);
  if (!capture_pipe)
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"No capture for %s\"}", lens);

  GstSample *sample = capture_pipeline_grab_snapshot(capture_pipe);
  if (!sample)
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"No frame available\"}");

  GstCaps *caps = gst_sample_get_caps(sample);
  GstBuffer *buf = gst_sample_get_buffer(sample);
  if (!caps || !buf) {
    gst_sample_unref(sample);
    return g_strdup("{\"status\":\"error\",\"message\":\"Invalid sample (no "
                    "caps or buffer)\"}");
  }

  gchar *caps_str = gst_caps_to_string(caps);

  /* Ensure parent directory exists */
  char *dir = g_path_get_dirname(path);
  if (dir) {
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }

  char launch_str[1024];
  snprintf(launch_str, sizeof(launch_str),
           "appsrc name=src caps=\"%s\" ! videoconvert ! jpegenc quality=90 ! "
           "filesink location=\"%s\"",
           caps_str, path);
  g_free(caps_str);

  GError *snap_err = NULL;
  GstElement *snap_pipe = gst_parse_launch(launch_str, &snap_err);
  if (!snap_pipe) {
    g_warning("Snapshot pipeline creation failed: %s",
              snap_err ? snap_err->message : "unknown");
    if (snap_err)
      g_error_free(snap_err);
    gst_sample_unref(sample);
    return g_strdup("{\"status\":\"error\",\"message\":\"Snapshot pipeline "
                    "creation failed\"}");
  }

  GstElement *appsrc = gst_bin_get_by_name(GST_BIN(snap_pipe), "src");
  if (!appsrc) {
    gst_element_set_state(snap_pipe, GST_STATE_NULL);
    gst_object_unref(snap_pipe);
    gst_sample_unref(sample);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"Snapshot appsrc not found\"}");
  }

  gst_element_set_state(snap_pipe, GST_STATE_PLAYING);

  GstFlowReturn ret;
  g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
  g_signal_emit_by_name(appsrc, "end-of-stream", &ret);
  gst_object_unref(appsrc);

  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(snap_pipe));
  GstMessage *msg = gst_bus_timed_pop_filtered(
      bus, 2 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    GError *e2;
    gchar *dbg;
    gst_message_parse_error(msg, &e2, &dbg);
    g_warning("Snapshot pipeline error: %s (%s)", e2->message, dbg);
    g_error_free(e2);
    g_free(dbg);
  }
  if (msg)
    gst_message_unref(msg);
  gst_object_unref(bus);

  gst_element_set_state(snap_pipe, GST_STATE_NULL);
  gst_object_unref(snap_pipe);
  gst_sample_unref(sample);

  /* Verify file was created */
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    return g_strdup_printf("{\"status\":\"error\",\"message\":\"Snapshot file "
                           "not created at %s\"}",
                           path);
  }

  return g_strdup_printf("{\"status\":\"captured\",\"file\":\"%s\"}", path);
}

/* ── Schedule JSON Parser Helper ── */
static char *_parse_schedule_config(RecordingBranch *rec, JsonObject *obj) {
  const char *stype =
      json_object_get_string_member_with_default(obj, "schedule_type", "once");
  g_strlcpy(rec->sched.type, stype, sizeof(rec->sched.type));

  if (g_strcmp0(stype, "once") == 0) {
    const char *s_start =
        json_object_get_string_member(obj, "schedule_start_time");
    const char *s_end = json_object_get_string_member(obj, "schedule_end_time");
    if (s_start && s_end) {
      struct tm tms, tme;
      memset(&tms, 0, sizeof(tms));
      memset(&tme, 0, sizeof(tme));

      if (sscanf(s_start, "%d-%d-%d %d:%d:%d", &tms.tm_year, &tms.tm_mon,
                 &tms.tm_mday, &tms.tm_hour, &tms.tm_min, &tms.tm_sec) == 6 &&
          sscanf(s_end, "%d-%d-%d %d:%d:%d", &tme.tm_year, &tme.tm_mon,
                 &tme.tm_mday, &tme.tm_hour, &tme.tm_min, &tme.tm_sec) == 6) {

        tms.tm_year -= 1900;
        tms.tm_mon -= 1;
        tms.tm_isdst = -1;
        tme.tm_year -= 1900;
        tme.tm_mon -= 1;
        tme.tm_isdst = -1;

        rec->sched.real_start = mktime(&tms);
        rec->sched.real_end = mktime(&tme);

        if (rec->sched.real_end <= rec->sched.real_start) {
          return g_strdup(
              "schedule_end_time must be strictly after schedule_start_time");
        }
        return NULL;
      } else {
        return g_strdup("Invalid time format. Use YYYY-MM-DD HH:MM:SS");
      }
    }
    return g_strdup(
        "schedule_start_time and schedule_end_time required for type 'once'");
  } else if (g_strcmp0(stype, "daily") == 0) {
    const char *s_start = json_object_get_string_member(obj, "start_time");
    const char *s_end = json_object_get_string_member(obj, "end_time");
    if (s_start && s_end) {
      int sh, sm, ss, eh, em, es;
      if (sscanf(s_start, "%d:%d:%d", &sh, &sm, &ss) == 3 &&
          sscanf(s_end, "%d:%d:%d", &eh, &em, &es) == 3) {
        rec->sched.daily_start_sec = sh * 3600 + sm * 60 + ss;
        rec->sched.daily_end_sec = eh * 3600 + em * 60 + es;
        return NULL;
      } else {
        return g_strdup("Invalid time format. Use HH:MM:SS");
      }
    }
    return g_strdup("start_time and end_time required for type 'daily'");
  }
  return g_strdup("Unknown schedule_type");
}

/* ── Extract Branch Config From JSON ── */
static BranchConfig *
_parse_recording_branch_config(UnifiedEngine *engine, const char *lens,
                               const char *tier, const char *mode,
                               JsonObject *obj, char **err_msg) {
  BranchConfig *rec_cfg = g_new0(BranchConfig, 1);

  LensInfo *lens_info = g_hash_table_lookup(engine->lenses, lens);

  if (g_strcmp0(mode, "event") == 0 || g_strcmp0(mode, "scheduled") == 0) {
    if (lens_info && lens_info->rec_defaults) {
      *rec_cfg = *(lens_info->rec_defaults);
    } else if (g_strcmp0(mode, "event") == 0) {
      /* Legacy fallback for AI if no defaults in config */
      rec_cfg->width = 1280;
      rec_cfg->height = 720;
      g_strlcpy(rec_cfg->resolution, "1280x720", sizeof(rec_cfg->resolution));
      rec_cfg->fps = 25;
      rec_cfg->bitrate = 2000000;
      g_strlcpy(rec_cfg->codec, "h264", sizeof(rec_cfg->codec));
    } else {
      /* Scheduled legacy: sync with live branch */
      goto sync_with_live;
    }
  } else {
  sync_with_live:;
    char lookup_key[128];
    snprintf(lookup_key, sizeof(lookup_key), "%s/%s", lens, tier);
    BranchConfig *branch_config_ptr =
        g_hash_table_lookup(engine->branch_configs, lookup_key);
    if (!branch_config_ptr) {
      g_free(rec_cfg);
      *err_msg = g_strdup_printf(
          "{\"status\":\"error\",\"message\":\"Branch '%s' not configured\"}",
          lookup_key);
      return NULL;
    }
    *rec_cfg = *branch_config_ptr; /* Copy defaults */
  }
  if (json_object_has_member(obj, "resolution")) {
    const char *res = json_object_get_string_member(obj, "resolution");
    sscanf(res, "%dx%d", &rec_cfg->width, &rec_cfg->height);
  }
  if (json_object_has_member(obj, "fps")) {
    rec_cfg->fps = (int)json_object_get_int_member(obj, "fps");
  }
  if (json_object_has_member(obj, "bitrate")) {
    rec_cfg->bitrate = (int)json_object_get_int_member(obj, "bitrate");
  }
  if (json_object_has_member(obj, "encoder")) {
    g_strlcpy(rec_cfg->codec, json_object_get_string_member(obj, "encoder"),
              sizeof(rec_cfg->codec));
  }
  if (json_object_has_member(obj, "bitrate_mode")) {
    g_strlcpy(rec_cfg->bitrate_mode,
              json_object_get_string_member(obj, "bitrate_mode"),
              sizeof(rec_cfg->bitrate_mode));
  }
  /* NTP overlay: only enabled when user explicitly requests it */
  if (json_object_has_member(obj, "ntp_overlay")) {
    rec_cfg->ntp_overlay = json_object_get_boolean_member(obj, "ntp_overlay");
  }
  return rec_cfg;
}

/* ── Enforce Edge Recording Restrictions (Time-Aware) ── */
static char *_check_recording_locks(UnifiedEngine *engine, const char *lens,
                                    const char *tier,
                                    RecordingBranch *new_rec) {
  GHashTableIter iter;
  gpointer rec_key, rec_val;
  g_hash_table_iter_init(&iter, engine->recordings);

  while (g_hash_table_iter_next(&iter, &rec_key, &rec_val)) {
    RecordingBranch *existing = (RecordingBranch *)rec_val;
    if (g_strcmp0(existing->lens, lens) == 0 &&
        g_strcmp0(existing->tier, tier) == 0) {

      /* Standard Check: Cannot have two continuous/event recordings on same
       * tier */
      if (g_strcmp0(new_rec->mode, "scheduled") != 0 &&
          g_strcmp0(existing->mode, "scheduled") != 0) {
        return g_strdup_printf(
            "{\"status\":\"error\",\"message\":\"Already have a %s recording "
            "active on %s/%s. Please stop it first.\"}",
            existing->mode, lens, tier);
      }

      /* Schedule Overlap Check (The user requirement) */
      if (recording_is_overlapping(new_rec, existing)) {
        return g_strdup_printf("{\"status\":\"error\",\"message\":\"Time "
                               "Conflict! New schedule overlaps with existing "
                               "task (%s) on %s/%s. Action: Modify or remove "
                               "existing schedule.\"}",
                               existing->sched.type, lens, tier);
      }
    }
  }
  return NULL;
}

/* ── StartRecording ── */
char *engine_start_recording(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");
  const char *mode =
      json_object_get_string_member_with_default(obj, "mode", "continuous");

  const char *tier =
      json_object_get_string_member_with_default(obj, "branch", "main");
  if (g_strcmp0(mode, "event") == 0) {
    /* [FIX-4] Event-mode recording was hardcoded to tier "ai" — a tier that
     * is never registered in branch_configs and does not exist in the capture
     * pipeline.  This caused _parse_recording_branch_config to return NULL
     * ("Branch 'lens1/ai' not configured") and silently kill every
     * AI-triggered recording.
     *
     * The correct default is "sub": it is always configured alongside "main",
     * runs at lower resolution/bitrate (appropriate for event clips), and
     * the caller can still override it by passing "branch": "main" in the
     * JSON if they want full-resolution event recording. */
    tier = json_object_has_member(obj, "branch")
               ? json_object_get_string_member(obj, "branch")
               : "sub";
  }

  const char *dir = json_object_get_string_member_with_default(
      obj, "output_dir", DEFAULT_DATA_DIR);

  /* [FIX-6] Validate output path before any directory creation. */
  if (!_is_safe_output_path(dir)) {
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":"
        "\"output_dir '%s' is outside the permitted recording area (%s). "
        "Refusing to create directories.\"}",
        dir, DEFAULT_DATA_DIR);
  }
  int idle_timeout =
      (int)json_object_get_int_member_with_default(obj, "idle_timeout", 120);
  int max_seg =
      (int)json_object_get_int_member_with_default(obj, "max_segment_sec", 300);

  if (g_strcmp0(mode, "scheduled") == 0) {
    json_object_get_string_member_with_default(obj, "schedule_type", "once");
  }

  char key[128];
  snprintf(key, sizeof(key), "%s/%s", lens, tier);

  /* 1. Branch Configuration Decoding */
  char *cfg_err = NULL;
  BranchConfig *rec_cfg =
      _parse_recording_branch_config(engine, lens, tier, mode, obj, &cfg_err);
  if (!rec_cfg) {
    g_object_unref(parser);
    return cfg_err;
  }

  /* 2. System Hardware Validation (Pre-requisite for branch creation) */
  CapturePipeline *capture_pipe = _get_capture(engine, lens);
  if (!capture_pipe) {
    g_free(rec_cfg);
    g_object_unref(parser);
    return g_strdup("{\"status\":\"error\",\"message\":\"No capture node "
                    "active for target.\"}");
  }

  /* 3. Create Branch Object (Used for overlap checking) */
  RecordingBranch *rec =
      recording_branch_new(engine, capture_pipe, lens, tier, dir, rec_cfg, mode,
                           idle_timeout, max_seg);
  branch_config_free(rec_cfg); /* Snapshot taken by recording_branch_new,
                                  original no longer needed */

  if (g_strcmp0(mode, "scheduled") == 0) {
    char *err_msg = _parse_schedule_config(rec, obj);
    if (err_msg) {
      recording_branch_free(rec);
      g_object_unref(parser);
      char *resp =
          g_strdup_printf("{\"status\":\"error\",\"message\":\"%s\"}", err_msg);
      g_free(err_msg);
      return resp;
    }
  }

  /* 4. Temporal Overlap & Concurrency Validation */
  g_mutex_lock(&engine->scheduler_mutex);
  char *lock_err = _check_recording_locks(engine, lens, tier, rec);
  if (lock_err) {
    g_mutex_unlock(&engine->scheduler_mutex);
    recording_branch_free(rec);
    g_object_unref(parser);
    return lock_err;
  }

  /* 5. Start Recording */
  if (recording_branch_start(rec)) {
    g_hash_table_insert(engine->recordings, g_strdup(key), rec);

    /* Nudge the scheduler thread to re-evaluate the next event */
    g_cond_signal(&engine->scheduler_cond);
    g_mutex_unlock(&engine->scheduler_mutex);

    char *resp = g_strdup_printf(
        "{\"status\":\"recording\",\"lens\":\"%s\",\"branch\":\"%s\"}", lens,
        tier);

    _save_user_config(engine);
    g_object_unref(parser);
    return resp;
  }
  g_mutex_unlock(&engine->scheduler_mutex);

  char *err_resp = g_strdup_printf(
      "{\"status\":\"error\",\"message\":\"Failed to start recording %s\"}",
      key);
  g_object_unref(parser);
  recording_branch_free(rec);
  return err_resp;
}

/* ── StopRecording ── */
char *engine_stop_recording(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");
  const char *tier =
      json_object_get_string_member_with_default(obj, "branch", "main");

  char key[128];
  snprintf(key, sizeof(key), "%s/%s", lens, tier);

  g_mutex_lock(&engine->scheduler_mutex);
  RecordingBranch *rec = g_hash_table_lookup(engine->recordings, key);
  if (!rec) {
    g_mutex_unlock(&engine->scheduler_mutex);
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"No recording for %s\"}", key);
  }

  g_hash_table_remove(engine->recordings, key);

  /* Nudge scheduler to re-calculate now that a slot is free */
  g_cond_signal(&engine->scheduler_cond);
  g_mutex_unlock(&engine->scheduler_mutex);

  g_object_unref(parser);
  return g_strdup("{\"status\":\"stopped\"}");
}

/* ── StartHLS ── */
char *engine_start_hls(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");
  const char *tier =
      json_object_get_string_member_with_default(obj, "branch", "main");
  const char *dir = json_object_get_string_member_with_default(
      obj, "output_dir", DEFAULT_DATA_DIR "/hls");

  /* [FIX-6] Same path allowlist check as engine_start_recording. */
  if (!_is_safe_output_path(dir)) {
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":"
        "\"output_dir '%s' is outside the permitted area (%s).\"}",
        dir, DEFAULT_DATA_DIR);
  }

  char key[128];
  snprintf(key, sizeof(key), "%s/%s", lens, tier);

  if (g_hash_table_contains(engine->hls_generators, key)) {
    HLSGenerator *existing_hls =
        g_hash_table_lookup(engine->hls_generators, key);
    hls_generator_refresh(existing_hls);
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"active\",\"message\":\"HLS refreshed for %s\"}", key);
  }

  /* Ensure capture exists */
  CapturePipeline *capture_pipe = _get_capture(engine, lens);
  if (!capture_pipe) {
    g_object_unref(parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"No capture for lens\"}");
  }

  /* Get branch config */
  BranchConfig *branch_config_ptr =
      g_hash_table_lookup(engine->branch_configs, key);
  if (!branch_config_ptr) {
    g_object_unref(parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"Branch not configured\"}");
  }

  /* Look up lens lens_info for Cairo capability and combine with branch config
   */
  LensInfo *lens_info = g_hash_table_lookup(engine->lenses, lens);
  gboolean cairo_enabled;
  _compute_effective_cairo_enabled(
      lens, tier,
      (lens_info ? lens_info->cairo_capable : TRUE) &&
          branch_config_ptr->overlay_enabled,
      &cairo_enabled, branch_config_ptr->ntp_overlay);

  /* We need to ACQUIRE the encoder before starting HLS generator */
  if (!stream_manager_acquire(engine->stream_mgr, capture_pipe, lens, tier,
                              branch_config_ptr, cairo_enabled)) {
    g_object_unref(parser);
    return g_strdup("{\"status\":\"error\",\"message\":\"Failed to acquire "
                    "shared encoder\"}");
  }

  HLSGenerator *hls = hls_generator_new(engine->stream_mgr, lens, tier,
                                        branch_config_ptr->codec, dir);
  hls_generator_start(hls);
  g_hash_table_insert(engine->hls_generators, g_strdup(key), hls);

  g_info("[HLS] Started HLS for %s at %s", key, dir);
  char *resp = g_strdup_printf(
      "{\"status\":\"started\",\"lens\":\"%s\",\"branch\":\"%s\"}", lens, tier);
  g_object_unref(parser);
  return resp;
}

/* ── StopHLS ── */
char *engine_stop_hls(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");
  const char *tier =
      json_object_get_string_member_with_default(obj, "branch", "main");

  char key[128];
  snprintf(key, sizeof(key), "%s/%s", lens, tier);

  if (g_hash_table_remove(engine->hls_generators, key)) {
    /* release encoder */
    stream_manager_release(engine->stream_mgr, lens, tier);
    g_info("[HLS] Stopped HLS for %s", key);
    g_object_unref(parser);
    return g_strdup("{\"status\":\"stopped\"}");
  }

  g_object_unref(parser);
  return g_strdup(
      "{\"status\":\"error\",\"message\":\"HLS not running for branch\"}");
}

/* ── GetStatus (detailed, per-lens) ── */
char *engine_get_status(UnifiedEngine *engine) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "total_clients");
  json_builder_add_int_value(b, engine->total_clients);
  json_builder_set_member_name(b, "lenses");
  json_builder_begin_object(b);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, engine->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lens_id = (const char *)key;
    LensInfo *lens_info = (LensInfo *)value;

    json_builder_set_member_name(b, lens_id);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "device");
    json_builder_add_string_value(b, lens_info->device);
    json_builder_set_member_name(b, "overlay_enabled");
    json_builder_add_boolean_value(b, lens_info->overlay_enabled);

    /* Branches */
    json_builder_set_member_name(b, "branches");
    json_builder_begin_object(b);

    GHashTableIter cfg_iter;
    gpointer iter_key, iter_val;
    g_hash_table_iter_init(&cfg_iter, engine->branch_configs);
    while (g_hash_table_iter_next(&cfg_iter, &iter_key, &iter_val)) {
      const char *branch_config_key = (const char *)iter_key;
      if (!g_str_has_prefix(branch_config_key, lens_id))
        continue;
      BranchConfig *branch_config_ptr = (BranchConfig *)iter_val;
      const char *tier = strchr(branch_config_key, '/');
      if (tier)
        tier++;

      char mount[64];
      snprintf(mount, sizeof(mount), "/%s", branch_config_key);
      /* CapturePipeline *capture_pipe lookup removed (unused) */
      gboolean is_active = stream_manager_is_active(
          engine->stream_mgr, lens_id, tier ? tier : branch_config_key);

      json_builder_set_member_name(b, tier ? tier : branch_config_key);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "mount");
      json_builder_add_string_value(b, mount);
      json_builder_set_member_name(b, "resolution");
      json_builder_add_string_value(b, branch_config_ptr->resolution);
      json_builder_set_member_name(b, "fps");
      json_builder_add_int_value(b, branch_config_ptr->fps);
      json_builder_set_member_name(b, "encoder");
      json_builder_add_string_value(b, branch_config_ptr->codec);
      json_builder_set_member_name(b, "state");
      json_builder_add_string_value(b, is_active ? "active" : "idle");

      gboolean is_recording = FALSE;
      RecordingBranch *rec =
          g_hash_table_lookup(engine->recordings, branch_config_key);
      if (rec && rec->running)
        is_recording = TRUE;
      json_builder_set_member_name(b, "recording");
      json_builder_add_boolean_value(b, is_recording);
      json_builder_end_object(b);
    }
    json_builder_end_object(b); /* branches */
    json_builder_end_object(b); /* lens */
  }

  json_builder_end_object(b); /* lenses */
  json_builder_end_object(b); /* root */

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(b));
  char *result = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  g_object_unref(b);
  return result;
}

/* ── SetLogLevel ── */
char *engine_set_log_level(UnifiedEngine *engine, const char *level) {
  (void)engine;
  if (g_strcmp0(level, "DEBUG") == 0) {
    gst_debug_set_default_threshold(GST_LEVEL_DEBUG);
    g_app_log_level = G_LOG_LEVEL_DEBUG;
  } else if (g_strcmp0(level, "INFO") == 0) {
    gst_debug_set_default_threshold(GST_LEVEL_INFO);
    g_app_log_level = G_LOG_LEVEL_INFO;
  } else if (g_strcmp0(level, "WARNING") == 0) {
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);
    g_app_log_level = G_LOG_LEVEL_WARNING;
  } else if (g_strcmp0(level, "ERROR") == 0) {
    gst_debug_set_default_threshold(GST_LEVEL_ERROR);
    g_app_log_level = G_LOG_LEVEL_ERROR;
  } else if (g_strcmp0(level, "APP") == 0) {
    /* APP mode: Show all application logs, suppress GStreamer/V4L2/RTSP noise
     */
    gst_debug_set_default_threshold(
        GST_LEVEL_ERROR);                /* Silence GStreamer except errors */
    g_app_log_level = G_LOG_LEVEL_DEBUG; /* Allow all our app logs */
    g_app_only_mode = TRUE;
    g_info(
        "Engine log level set to APP-ONLY mode (GStreamer noise suppressed)");
    return g_strdup("{\"status\":\"ok\",\"log_level\":\"APP\"}");
  }
  g_app_only_mode = FALSE; /* Disable APP mode for all other levels */
  g_info("Engine log level dynamically set to: %s", level);
  return g_strdup_printf("{\"status\":\"ok\",\"log_level\":\"%s\"}", level);
}

/* ── AI Shared Memory Interface (Clean Architecture) ── */
char *engine_attach_shm(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }

  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");
  const char *socket_path = json_object_get_string_member(obj, "socket_path");

  if (!socket_path) {
    g_object_unref(parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"socket_path required\"}");
  }

  CapturePipeline *capture_pipe = _get_capture(engine, lens);
  if (!capture_pipe) {
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"No capture for lens %s\"}", lens);
  }

  /* Ensure the parent directory of the socket_path exists to prevent shmsink
   * startup failure */
  char *dir = g_path_get_dirname(socket_path);
  if (dir) {
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
  }

  /* Securely attach the SHM branch so the AI Engine can consume it natively */
  capture_pipeline_attach_ai_shm(capture_pipe, socket_path);

  g_info("[AI] Feed attached for %s. SHM socket: %s", lens, socket_path);

  char *resp = g_strdup_printf(
      "{\"status\":\"ok\",\"message\":\"AI SHM attached\",\"socket\":\"%s\"}",
      socket_path);

  g_object_unref(parser);
  return resp;
}

char *engine_detach_shm(UnifiedEngine *engine, const char *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(parser);
    return resp;
  }

  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");

  CapturePipeline *capture_pipe = _get_capture(engine, lens);
  if (capture_pipe) {
    capture_pipeline_detach_ai_shm(capture_pipe);
    g_info("[AI] Feed detached for %s.", lens);
  }

  g_object_unref(parser);
  return g_strdup("{\"status\":\"ok\",\"message\":\"AI SHM detached\"}");
}

/* ── Toggle Overlay Interface ── */
char *engine_toggle_overlay(UnifiedEngine *engine, const char *json) {
  JsonParser *json_parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(json_parser, json, -1, &err)) {
    char *response = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(json_parser);
    return response;
  }

  JsonObject *request_obj =
      json_node_get_object(json_parser_get_root(json_parser));
  const char *lens_id = json_object_get_string_member_with_default(
      request_obj, "lens_id", "lens1");
  /* Optional tier_name parameter for per-stream overlay control */
  const char *tier_name = NULL;
  if (json_object_has_member(request_obj, "tier_name")) {
    tier_name = json_object_get_string_member(request_obj, "tier_name");
  }

  if (!json_object_has_member(request_obj, "overlay_on")) {
    g_object_unref(json_parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"'overlay_on' boolean required\"}");
  }
  gboolean overlay_on =
      json_object_get_boolean_member(request_obj, "overlay_on");

  CapturePipeline *capture_pipe = _get_capture(engine, lens_id);
  if (!capture_pipe) {
    char *response = g_strdup_printf("{\"status\":\"error\",\"message\":\"No "
                                     "capture pipeline for lens_id '%s'\"}",
                                     lens_id);
    g_object_unref(json_parser);
    return response;
  }

  int overlays_toggled = 0;
  g_mutex_lock(&capture_pipe->branch_mutex);

  GHashTableIter branch_iter;
  gpointer iter_key, iter_val;
  g_hash_table_iter_init(&branch_iter, capture_pipe->dynamic_branches);

  while (g_hash_table_iter_next(&branch_iter, &iter_key, &iter_val)) {
    const char *pipeline_branch_name = (const char *)iter_key;
    DynamicBranch *active_branch = (DynamicBranch *)iter_val;

    /* If a specific tier_name was requested, skip others */
    if (tier_name && !g_str_has_suffix(pipeline_branch_name, tier_name)) {
      continue;
    }

    /* Guard: elements array may be NULL during async teardown */
    if (!active_branch->elements) {
      continue;
    }

    /* Find cairooverlay element and update its QData */
    for (int i = 0; i < active_branch->num_elements; i++) {
      if (active_branch->elements[i]) {
        GstElementFactory *f =
            gst_element_get_factory(active_branch->elements[i]);
        if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)),
                           "cairooverlay") == 0) {
          g_object_set_data(G_OBJECT(active_branch->elements[i]),
                            "overlay_enabled", GINT_TO_POINTER(overlay_on));
          overlays_toggled++;
          g_info("[AI] Overlay on tier_name '%s' set to %s",
                 pipeline_branch_name, overlay_on ? "ON" : "OFF");
        }
      }
    }
  }
  g_mutex_unlock(&capture_pipe->branch_mutex);

  if (overlays_toggled == 0 && tier_name) {
    g_warning(
        "[AI] No cairooverlay found for %s/%s — tier_name may not exist or "
        "has no overlay",
        lens_id, tier_name);
  }
  g_info("[AI] Overlay toggle for %s/%s: %d overlays set to %s", lens_id,
         tier_name ? tier_name : "all", overlays_toggled,
         overlay_on ? "ON" : "OFF");

  _warn_overlay_toggle_limitations(engine, lens_id, tier_name, overlay_on,
                                   capture_pipe);

  char *response;
  if (overlays_toggled == 0) {
    response = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Cairo overlay is removed from the "
        "pipeline. Please add it via ConfigureLens first.\"}");
  } else {
    response =
        g_strdup_printf("{\"status\":\"ok\",\"lens_id\":\"%s\",\"tier_name\":"
                        "\"%s\",\"overlay_enabled\":%s,\"count\":%d}",
                        lens_id, tier_name ? tier_name : "all",
                        overlay_on ? "true" : "false", overlays_toggled);
  }
  g_object_unref(json_parser);
  return response;
}

/* ── ToggleNTP ── */
char *engine_toggle_ntp(UnifiedEngine *engine, const char *json) {
  JsonParser *json_parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(json_parser, json, -1, &err)) {
    char *resp = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Invalid JSON: %s\"}",
        err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_object_unref(json_parser);
    return resp;
  }

  JsonObject *request_obj =
      json_node_get_object(json_parser_get_root(json_parser));
  const char *lens_id = json_object_get_string_member_with_default(
      request_obj, "lens_id", "lens1");
  const char *tier_name = NULL;
  if (json_object_has_member(request_obj, "tier_name")) {
    tier_name = json_object_get_string_member(request_obj, "tier_name");
  }

  if (!json_object_has_member(request_obj, "ntp_on")) {
    g_object_unref(json_parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"'ntp_on' boolean required\"}");
  }
  gboolean ntp_on = json_object_get_boolean_member(request_obj, "ntp_on");

  /* Update BranchConfig to persist for next connect */
  if (tier_name) {
    char lookup_key[128];
    snprintf(lookup_key, sizeof(lookup_key), "%s/%s", lens_id, tier_name);
    BranchConfig *branch_config_ptr =
        g_hash_table_lookup(engine->branch_configs, lookup_key);
    if (branch_config_ptr) {
      branch_config_ptr->ntp_overlay = ntp_on;
    }
  }

  CapturePipeline *capture_pipe = _get_capture(engine, lens_id);
  if (!capture_pipe) {
    g_object_unref(json_parser);
    return g_strdup_printf(
        "{\"status\":\"config_updated\",\"note\":\"No capture pipeline for "
        "lens_id '%s', configuration saved.\"}",
        lens_id);
  }

  int overlays_toggled = 0;
  g_mutex_lock(&capture_pipe->branch_mutex);

  GHashTableIter branch_iter;
  gpointer iter_key, iter_val;
  g_hash_table_iter_init(&branch_iter, capture_pipe->dynamic_branches);

  while (g_hash_table_iter_next(&branch_iter, &iter_key, &iter_val)) {
    const char *pipeline_branch_name = (const char *)iter_key;
    DynamicBranch *active_branch = (DynamicBranch *)iter_val;

    /* If a specific tier_name was requested, skip others */
    if (tier_name && !g_str_has_suffix(pipeline_branch_name, tier_name)) {
      continue;
    }

    if (!active_branch->elements) {
      continue;
    }

    /* Find clockoverlay element and toggle its 'silent' property */
    for (int i = 0; i < active_branch->num_elements; i++) {
      if (active_branch->elements[i]) {
        GstElementFactory *f =
            gst_element_get_factory(active_branch->elements[i]);
        if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)),
                           "clockoverlay") == 0) {
          g_object_set(active_branch->elements[i], "silent", !ntp_on, NULL);
          overlays_toggled++;
          g_info("[NTP] Overlay on tier_name '%s' set to %s",
                 pipeline_branch_name, ntp_on ? "ON" : "OFF");
        }
      }
    }
  }

  g_mutex_unlock(&capture_pipe->branch_mutex);
  _save_user_config(engine);
  g_object_unref(json_parser);

  _warn_ntp_toggle_limitations(lens_id, tier_name, ntp_on, overlays_toggled);

  if (overlays_toggled == 0) {
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"NTP Clock overlay is removed from "
        "the pipeline. Please add it via ConfigureLens first.\"}");
  }

  return g_strdup_printf(
      "{\"status\":\"success\",\"lens_id\":\"%s\",\"tier_name\":"
      "\"%s\",\"ntp_on\":%s,\"overlays_toggled\":%d}",
      lens_id, tier_name ? tier_name : "all", ntp_on ? "true" : "false",
      overlays_toggled);
}

/* ── SaveCurrentConfig ── */
gboolean engine_save_config(UnifiedEngine *engine) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, engine->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lens_id = (const char *)key;
    json_builder_set_member_name(b, lens_id);
    json_builder_begin_object(b);

    /* RTSP configs */
    json_builder_set_member_name(b, "rtsp");
    json_builder_begin_object(b);
    GHashTableIter cfg_iter;
    gpointer iter_key, iter_val;
    g_hash_table_iter_init(&cfg_iter, engine->branch_configs);
    while (g_hash_table_iter_next(&cfg_iter, &iter_key, &iter_val)) {
      const char *branch_config_key = (const char *)iter_key;
      if (!g_str_has_prefix(branch_config_key, lens_id))
        continue;
      BranchConfig *branch_config_ptr = (BranchConfig *)iter_val;
      const char *tier = strchr(branch_config_key, '/');
      if (tier)
        tier++;
      json_builder_set_member_name(b, tier ? tier : "main");
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "resolution");
      json_builder_add_string_value(b, branch_config_ptr->resolution);
      json_builder_set_member_name(b, "fps");
      json_builder_add_int_value(b, branch_config_ptr->fps);
      json_builder_set_member_name(b, "encoder");
      json_builder_add_string_value(b, branch_config_ptr->codec);
      json_builder_end_object(b);
    }
    json_builder_end_object(b); /* rtsp */

    json_builder_end_object(b); /* lens */
  }

  json_builder_end_object(b);

  /* Write to config file as user_overrides */
  JsonParser *parser = json_parser_new();
  gchar *existing = NULL;
  gsize elen;
  JsonObject *root = NULL;
  if (g_file_get_contents(engine->config_path, &existing, &elen, NULL)) {
    if (json_parser_load_from_data(parser, existing, elen, NULL)) {
      root = json_node_get_object(json_parser_get_root(parser));
    }
  } else if (g_file_get_contents(DEFAULT_CONFIG_PATH, &existing, &elen, NULL)) {
    if (json_parser_load_from_data(parser, existing, elen, NULL)) {
      root = json_node_get_object(json_parser_get_root(parser));
    }
  }
  g_free(existing);

  /* Build final config */
  JsonBuilder *wb = json_builder_new();
  json_builder_begin_object(wb);
  json_builder_set_member_name(wb, "enabled");
  json_builder_add_boolean_value(wb, TRUE);
  if (root && json_object_has_member(root, "manufacturer_defaults")) {
    json_builder_set_member_name(wb, "manufacturer_defaults");
    json_builder_add_value(wb, json_node_copy(json_object_get_member(
                                   root, "manufacturer_defaults")));
  }
  json_builder_set_member_name(wb, "user_overrides");
  json_builder_add_value(wb, json_node_copy(json_builder_get_root(b)));
  json_builder_end_object(wb);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, json_builder_get_root(wb));
  gboolean ok = json_generator_to_file(gen, engine->config_path, NULL);

  g_object_unref(gen);
  g_object_unref(wb);
  g_object_unref(b);
  g_object_unref(parser);

  if (ok)
    g_info("Saved user config to %s", engine->config_path);
  return ok;
}

/* ── Periodic DOT Dump (120s) ── */
static gboolean _dump_all_dots(gpointer data) {
  UnifiedEngine *engine = (UnifiedEngine *)data;

  GDateTime *now = g_date_time_new_now_local();
  char *ts = g_date_time_format(now, "%H%M%S");
  g_date_time_unref(now);

  g_mutex_lock(&engine->config_mutex);

  GHashTableIter iter;
  gpointer key, value;

  /* ── 1. Capture Pipelines ── */
  g_hash_table_iter_init(&iter, engine->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lens_name = (const char *)key;
    LensInfo *lens_info = (LensInfo *)value;
    CapturePipeline *capture_pipe =
        g_hash_table_lookup(engine->captures, lens_info->device);

    if (!capture_pipe)
      continue;

    guint n_branches = g_hash_table_size(capture_pipe->dynamic_branches);
    char dot_name[256];
    /* Use lens_name instead of device path to avoid slashes in the filename
     * [FIX-3] */
    snprintf(dot_name, sizeof(dot_name), "FULL_%s_br%u_%s", lens_name,
             n_branches, ts);
    capture_pipeline_dump_dot(capture_pipe, dot_name);

    g_info("  [%s] %u branches, cairo=%s, ai_coords=%d overlay=%s", lens_name,
           n_branches, capture_pipe->ai.cairo_capable ? "yes" : "no",
           capture_pipe->ai.num_coords,
           capture_pipe->ai.overlay_enabled ? "on" : "off");
  }

  /* ── 2. HLS Pipelines ── */
  if (engine->hls_generators) {
    GHashTableIter hls_iter;
    gpointer hls_node_key, hls_node_val;
    g_hash_table_iter_init(&hls_iter, engine->hls_generators);
    while (g_hash_table_iter_next(&hls_iter, &hls_node_key, &hls_node_val)) {
      HLSGenerator *hls_gen = (HLSGenerator *)hls_node_val;
      if (!hls_gen)
        continue;
      GstElement *hls_pipeline = hls_generator_get_pipeline(hls_gen);
      if (hls_pipeline && GST_IS_ELEMENT(hls_pipeline)) {
        char dot_filename[128];
        snprintf(dot_filename, sizeof(dot_filename), "HLS_%s_%s",
                 (const char *)hls_node_key, ts);
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(hls_pipeline),
                                  GST_DEBUG_GRAPH_SHOW_ALL, dot_filename);
      }
    }
  }

  /* ── 3. RTSP Pipelines ── */
  if (engine->rtsp_server) {
    GstRTSPMountPoints *mount_points =
        gst_rtsp_server_get_mount_points(engine->rtsp_server);
    if (mount_points) {
      GHashTableIter br_config_iter;
      gpointer branch_node_key, branch_node_val;
      g_hash_table_iter_init(&br_config_iter, engine->branch_configs);
      while (g_hash_table_iter_next(&br_config_iter, &branch_node_key,
                                    &branch_node_val)) {
        char rtsp_mount_path[128];
        snprintf(rtsp_mount_path, sizeof(rtsp_mount_path), "/%s",
                 (const char *)branch_node_key);
        GstRTSPMediaFactory *media_factory =
            gst_rtsp_mount_points_match(mount_points, rtsp_mount_path, NULL);
        if (media_factory) {
          lazy_rtsp_factory_dump_dot(media_factory, ts);
          g_object_unref(media_factory);
        }
      }
      g_object_unref(mount_points);
    }
  }

  /* ── 4. AI Engine Pipeline (via D-Bus) ── */
  /* [FIX-1] Declare gres and err in this scope to fix compile error */
  if (engine->ai_proxy) {
    GError *err = NULL;
    GVariant *gres = g_dbus_proxy_call_sync(
        engine->ai_proxy, "DumpDOT", g_variant_new("(s)", ts),
        G_DBUS_CALL_FLAGS_NONE, 500, NULL, &err);

    if (gres) {
      g_variant_unref(gres);
    }
    if (err) {
      g_debug("AI DumpDOT skipped: %s", err->message);
      g_error_free(err);
    }
  }

  g_mutex_unlock(&engine->config_mutex);
  g_free(ts);
  return G_SOURCE_CONTINUE;
}

/* ══════════════════════════════════════════════════════════════
 *  System Stats (port of _log_system_stats — 15s periodic)
 * ══════════════════════════════════════════════════════════════ */

static gboolean _log_system_stats(gpointer data) {
  UnifiedEngine *engine = (UnifiedEngine *)data;

  /* ── 1. Reap Idle HLS Generators ── */
  GHashTableIter hls_iter;
  gpointer hls_id, hls_gen;
  GList *hls_to_remove = NULL;

  g_hash_table_iter_init(&hls_iter, engine->hls_generators);
  while (g_hash_table_iter_next(&hls_iter, &hls_id, &hls_gen)) {
    HLSGenerator *hls = (HLSGenerator *)hls_gen;
    if (hls_generator_get_idle_time_sec(hls) > 30) {
      g_info("[HLS Reaper] Reaping idle HLS stream for %s (no viewers for 30s)",
             (const char *)hls_id);
      hls_to_remove =
          g_list_append(hls_to_remove, g_strdup((const char *)hls_id));
    }
  }

  for (GList *l = hls_to_remove; l; l = l->next) {
    const char *key = (const char *)l->data;
    g_hash_table_remove(engine->hls_generators, key);
    /* Release StreamManager encoder — but only if the stream is still alive.
     * force_teardown from a codec rebuild may have already cleaned it up. */
    char **parts = g_strsplit(key, "/", 2);
    if (parts[0] && parts[1]) {
      if (stream_manager_is_active(engine->stream_mgr, parts[0], parts[1])) {
        stream_manager_release(engine->stream_mgr, parts[0], parts[1]);
      } else {
        g_debug("[HLS Reaper] Stream '%s' already torn down, skipping release",
                key);
      }
    }
    g_strfreev(parts);
    g_free(l->data);
  }
  g_list_free(hls_to_remove);

  /* ── 2. Standard Logging ── */
  g_info("==== UNIFIED ENGINE STATUS ====");
  g_info("Total RTSP Clients: %d", engine->total_clients);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, engine->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lens_id = (const char *)key;
    CapturePipeline *capture_pipe = _get_capture(engine, lens_id);
    int dyn_count = capture_pipe
                        ? (int)g_hash_table_size(capture_pipe->dynamic_branches)
                        : 0;
    g_info(" Lens [%s] → Dynamic branches: %d", lens_id, dyn_count);
  }
  _audit_overlay_pipeline_elements(engine);
  g_info("================================");
  return G_SOURCE_CONTINUE;
}

/* ══════════════════════════════════════════════════════════════
 *  RTSP Client Tracking (port of _on_rtsp_client_connected etc.)
 * ══════════════════════════════════════════════════════════════ */

static void _on_rtsp_client_connected(GstRTSPServer *server,
                                      GstRTSPClient *client, gpointer data) {
  (void)server;
  (void)client;
  (void)data;
  g_debug("RTSP: New TCP connection established");
}

static void _on_session_removed(GstRTSPSessionPool *pool,
                                GstRTSPSession *session, gpointer data) {
  (void)pool;
  UnifiedEngine *engine = (UnifiedEngine *)data;
  const gchar *sid = gst_rtsp_session_get_sessionid(session);

  gchar *mount = g_hash_table_lookup(engine->session_mounts, sid);
  if (mount) {
    gpointer count_ptr = g_hash_table_lookup(engine->active_branches, mount);
    int count = GPOINTER_TO_INT(count_ptr);
    g_hash_table_insert(engine->active_branches, g_strdup(mount),
                        GINT_TO_POINTER(count > 0 ? count - 1 : 0));
    engine->total_clients =
        engine->total_clients > 0 ? engine->total_clients - 1 : 0;
    g_hash_table_remove(engine->session_mounts, sid);
  }
}

/* ══════════════════════════════════════════════════════════════
 *  Default Config Application (port of _apply_defaults)
 * ══════════════════════════════════════════════════════════════ */

/* [CLEANUP]: _apply_defaults moved to subsystems/sys_config.c */

/* ══════════════════════════════════════════════════════════════
 *  Signal Handler
 * ══════════════════════════════════════════════════════════════ */

static void _signal_handler(int signum) {
  (void)signum;
  if (g_engine && g_engine->loop) {
    g_main_loop_quit(g_engine->loop);
  }
}

/* ══════════════════════════════════════════════════════════════
 *  Custom Dynamic Log Handler
 * ══════════════════════════════════════════════════════════════ */

static void _custom_log_handler(const gchar *log_domain,
                                GLogLevelFlags log_level, const gchar *message,
                                gpointer user_data) {
  (void)user_data;

  /* APP mode: only show our application logs, suppress GStreamer/V4L2/RTSP
   * noise */
  if (g_app_only_mode) {
    /* Allow all CRITICAL/ERROR from any domain */
    if (log_level <= G_LOG_LEVEL_WARNING) {
      g_log_default_handler(log_domain, log_level, message, NULL);
      return;
    }
    /* Suppress known noisy GStreamer domains */
    if (log_domain &&
        (g_str_has_prefix(log_domain, "GStreamer") ||
         g_str_has_prefix(log_domain, "Gst") || strstr(log_domain, "v4l2") ||
         strstr(log_domain, "rtsp") || strstr(log_domain, "rtp"))) {
      return; /* silenced */
    }
    /* Show all app-level logs (smartip_engine, default domain, NULL domain) */
    g_log_default_handler(log_domain, log_level, message, NULL);
    return;
  }

  /* Standard level filtering */
  if (log_level > g_app_log_level && log_level != G_LOG_LEVEL_MESSAGE) {
    return;
  }
  g_log_default_handler(log_domain, log_level, message, NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  ZMQ AI Coordinates Callback
 * ══════════════════════════════════════════════════════════════ */

/* ── AI Event Handler (Dispatcher for Coords & Alerts) ── */
static void _on_ai_event(const char *topic, const char *json_payload,
                         gpointer user_data) {
  UnifiedEngine *engine = (UnifiedEngine *)user_data;
  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_payload, -1, NULL)) {
    g_object_unref(parser);
    return;
  }

  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const char *lens =
      json_object_get_string_member_with_default(obj, "lens", "lens1");

  if (g_strcmp0(topic, TOPIC_AI_COORDS) == 0) {
    /* --- Coordinate Overlay Logic (Visuals Only) --- */
    CapturePipeline *capture_pipe = _get_capture(engine, lens);
    if (!capture_pipe) {
      g_object_unref(parser);
      return;
    }

    int ml_w = json_object_has_member(obj, "ml_w")
                   ? json_object_get_int_member(obj, "ml_w")
                   : 640;
    int ml_h = json_object_has_member(obj, "ml_h")
                   ? json_object_get_int_member(obj, "ml_h")
                   : 360;

    JsonArray *coords_array = json_object_has_member(obj, "coords")
                                  ? json_object_get_array_member(obj, "coords")
                                  : NULL;
    guint length = coords_array ? json_array_get_length(coords_array) : 0;

    g_mutex_lock(&capture_pipe->ai.mutex);
    capture_pipe->ai.ml_w = ml_w;
    capture_pipe->ai.ml_h = ml_h;

    /* [CPU-OPT-5] Reuse the existing allocation when the incoming detection
     * count matches.  At 25 fps with a stationary subject the count is
     * constant frame-to-frame, so this eliminates ~25 g_free/g_new0 pairs
     * per second per lens.  Only reallocate when the count actually changes. */
    if ((guint)capture_pipe->ai.num_coords != length) {
      g_free(capture_pipe->ai.coords);
      capture_pipe->ai.coords = length > 0 ? g_new0(AICoord, length) : NULL;
      capture_pipe->ai.num_coords = (int)length;
    }

    if (length > 0 && capture_pipe->ai.coords) {
      for (guint i = 0; i < length; i++) {
        JsonNode *elem_node = json_array_get_element(coords_array, i);
        if (!elem_node || !JSON_NODE_HOLDS_OBJECT(elem_node))
          continue;
        JsonObject *c_obj = json_node_get_object(elem_node);
        if (!c_obj)
          continue;

        if (json_object_has_member(c_obj, "bbox")) {
          JsonArray *bbox = json_object_get_array_member(c_obj, "bbox");
          if (bbox && json_array_get_length(bbox) >= 4) {
            capture_pipe->ai.coords[i].left =
                json_array_get_double_element(bbox, 0);
            capture_pipe->ai.coords[i].top =
                json_array_get_double_element(bbox, 1);
            capture_pipe->ai.coords[i].right =
                json_array_get_double_element(bbox, 2);
            capture_pipe->ai.coords[i].bottom =
                json_array_get_double_element(bbox, 3);
          }
        }
        capture_pipe->ai.coords[i].conf =
            json_object_get_double_member_with_default(c_obj, "conf", 0.0);
        const char *label = json_object_get_string_member_with_default(
            c_obj, "class_name", "unknown");
        g_strlcpy(capture_pipe->ai.coords[i].label, label,
                  sizeof(capture_pipe->ai.coords[i].label));
      }
    }
    g_mutex_unlock(&capture_pipe->ai.mutex);

  } else if (g_strcmp0(topic, TOPIC_ALERTS) == 0) {
    /* ── Option-B: TOPIC_ALERTS is the single authoritative trigger for both
     *    event-mode recording AND alert-driven snapshots.
     *
     *    The AI engine (ai_engine.py) already publishes TOPIC_ALERTS with a
     *    fully-enriched payload whenever a detection crosses
     * snapshot_threshold. The C engine subscribes to TOPIC_ALERTS (wired in
     * main() already). This handler is the only place that acts on alerts — no
     * duplicate signal path, no second ZMQ port needed.
     *
     *    [FIX-3] Previously this block existed but only poked recorders for
     *    ANY recording on the lens, ignoring mode.  Now it correctly filters
     *    to event-mode branches only, matching the intent of
     * recording_branch_poke.
     *
     *    [FIX-5] Autonomous snapshot: if the alert payload does NOT already
     *    carry a "snapshot" path (i.engine. the AI engine did not save one,
     * engine.g. ai_snapshots_enabled=false), the C engine saves its own JPEG
     * from the live appsink.  A per-lens cooldown prevents disk floods at full
     * fps.
     */

    const char *type =
        json_object_get_string_member_with_default(obj, "type", "detection");
    double conf = json_object_get_double_member_with_default(obj, "conf", 0.0);
    const char *ai_snap = json_object_has_member(obj, "snapshot")
                              ? json_object_get_string_member(obj, "snapshot")
                              : NULL;

    g_info("[AI-REC] Alert '%s' conf=%.2f lens=%s — poking event recorders%s",
           type, conf, lens, ai_snap ? " (AI snapshot present)" : "");

    /* ── A. Poke every event-mode recording branch for this lens ── */
    GHashTableIter iter;
    gpointer rec_key, rec_val;
    g_hash_table_iter_init(&iter, engine->recordings);
    while (g_hash_table_iter_next(&iter, &rec_key, &rec_val)) {
      RecordingBranch *rec = (RecordingBranch *)rec_val;
      if (g_strcmp0(rec->lens, lens) == 0 &&
          g_strcmp0(rec->mode, "event") == 0) {
        recording_branch_poke(rec);
      }
    }

    /* ── B. Autonomous C-engine snapshot (FIX-5) ──────────────────────────
     * Only triggered when:
     *   1. The AI engine did NOT already save a snapshot (ai_snap == NULL), OR
     *      the AI engine had ai_snapshots_enabled=false.
     *   2. The per-lens cooldown has elapsed (avoids writing a JPEG every frame
     *      at 25 fps when a person stands still in frame).
     *
     * Cooldown state is kept in a static GHashTable keyed by lens name.
     * Allocation is deferred to first use and protected by a static GMutex.
     * This avoids adding fields to UnifiedEngine for a single auxiliary
     * feature.
     */
    if (!ai_snap) {
/* Cooldown: one autonomous snapshot per lens per AI_SNAP_COOLDOWN_SEC. */
#define AI_SNAP_COOLDOWN_SEC 5

      static GMutex _snap_mtx;
      static GHashTable *_snap_times = NULL; /* lens → last snapshot time_t */
      static gboolean _snap_init = FALSE;

      g_mutex_lock(&_snap_mtx);
      if (!_snap_init) {
        _snap_times =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        _snap_init = TRUE;
      }

      time_t now_t = time(NULL);
      gpointer last_p = g_hash_table_lookup(_snap_times, lens);
      time_t last_t = last_p ? (time_t)GPOINTER_TO_INT(last_p) : 0;
      gboolean do_snap = (now_t - last_t) >= AI_SNAP_COOLDOWN_SEC;

      if (do_snap) {
        g_hash_table_insert(_snap_times, g_strdup(lens),
                            GINT_TO_POINTER((gint)now_t));
      }
      g_mutex_unlock(&_snap_mtx);

      if (do_snap) {
        /* Build timestamped path:
         * DEFAULT_DATA_DIR/snapshots/lens1_ai_20240101_120000.jpg */
        GDateTime *dt = g_date_time_new_now_local();
        char *ts_str = g_date_time_format(dt, "%Y%member_node%d_%H%M%S");
        g_date_time_unref(dt);

        char snap_path[512];
        snprintf(snap_path, sizeof(snap_path),
                 DEFAULT_DATA_DIR "/snapshots/%s_ai_%s.jpg", lens, ts_str);
        g_free(ts_str);

        /* engine_take_snapshot creates the directory, encodes via GStreamer,
         * and returns a JSON status string. It is safe to call from the ZMQ
         * callback thread because it builds a self-contained mini-pipeline
         * with its own GMainLoop bus wait. */
        char *snap_result = engine_take_snapshot(engine, lens, snap_path);
        if (snap_result) {
          g_info("[AI-SNAP] Auto-snapshot: %s", snap_result);
          g_free(snap_result);
        }
      }
    }
  }

  g_object_unref(parser);
}

/* ══════════════════════════════════════════════════════════════
 *  main() — Engine Entry Point
 * ══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
  /* CLI arguments */
  gint rtsp_port = DEFAULT_RTSP_PORT;
  gchar *config_path = NULL;

  GOptionEntry entries[] = {
      {"rtsp-port", 'p', 0, G_OPTION_ARG_INT, &rtsp_port, "RTSP port", "PORT"},
      {"config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "Config file",
       "PATH"},
      {NULL}};

  GOptionContext *opt_ctx =
      g_option_context_new("- SMART IP Edge Camera Engine");
  g_option_context_add_main_entries(opt_ctx, entries, NULL);
  g_option_context_add_group(opt_ctx, gst_init_get_option_group());

  GError *err = NULL;
  if (!g_option_context_parse(opt_ctx, &argc, &argv, &err)) {
    g_printerr("Option parsing failed: %s\n", err->message);
    g_error_free(err);
    return 1;
  }
  g_option_context_free(opt_ctx);

  /* Set DOT dir for pipeline graph export BEFORE gst_init */
  if (!g_getenv("GST_DEBUG_DUMP_DOT_DIR")) {
    const char *dot_dir = DEFAULT_DATA_DIR "/dot";
    g_mkdir_with_parents(dot_dir, 0755);
    g_setenv("GST_DEBUG_DUMP_DOT_DIR", dot_dir, TRUE);
  }

  /* Force GLib to produce all debug logs; our handler will filter them */
  g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
  g_log_set_default_handler(_custom_log_handler, NULL);

  gst_init(&argc, &argv);

  g_info("=== SMART IP Edge Camera Engine [STABLE-EDGE-v3.0] ===");
  g_info("RTSP port: %d", rtsp_port);

  /* ── Create Engine ── */
  UnifiedEngine *engine = g_new0(UnifiedEngine, 1);
  engine->loop = g_main_loop_new(NULL, FALSE);
  g_mutex_init(&engine->config_mutex);
  engine->rtsp_port = rtsp_port;
  g_strlcpy(engine->default_caps,
            "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1",
            sizeof(engine->default_caps));

  /* Lens registry (port of self._lenses) */
  engine->lenses =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  LensInfo *l1 = g_new0(LensInfo, 1);
  g_strlcpy(l1->device, "/dev/video3", sizeof(l1->device));
  l1->overlay_enabled = TRUE;
  l1->cairo_capable = TRUE;
  g_hash_table_insert(engine->lenses, g_strdup("lens1"), l1);
  LensInfo *l2 = g_new0(LensInfo, 1);
  g_strlcpy(l2->device, "/dev/video4", sizeof(l2->device));
  l2->overlay_enabled = TRUE;
  l2->cairo_capable = TRUE;
  g_hash_table_insert(engine->lenses, g_strdup("lens2"), l2);

  engine->captures = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)capture_pipeline_free);
  engine->branch_configs = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)branch_config_free);
  engine->recordings = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)recording_branch_free);
  engine->active_branches =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  engine->session_mounts =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  engine->total_clients = 0;

  /* Stream & HLS Management */
  /* Stream & HLS Management */
  engine->stream_mgr = stream_manager_new();
  engine->hls_generators = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)hls_generator_free);

  if (config_path) {
    g_strlcpy(engine->config_path, config_path, sizeof(engine->config_path));
    g_free(config_path);
  } else {
    /* Use user_config.json as the main write path, preserving the default
     * config. */
    g_strlcpy(engine->config_path, "config/user_config.json",
              sizeof(engine->config_path));
  }

  /* ZMQ Subscriber for AI Coordinates and Alerts */
  engine->mq_sub = mq_subscriber_new(ZMQ_DEFAULT_PORT);
  if (engine->mq_sub) {
    mq_subscriber_subscribe(engine->mq_sub, TOPIC_AI_COORDS);
    mq_subscriber_subscribe(engine->mq_sub, TOPIC_ALERTS);
    mq_subscriber_set_callback(engine->mq_sub, _on_ai_event, engine);
    mq_subscriber_start_background(engine->mq_sub);
  } else {
    g_warning("ZMQ Subscriber failed to initialize on port %d — AI coordinates "
              "will not be received",
              ZMQ_DEFAULT_PORT);
  }

  /* RTSP Server */
  engine->rtsp_server = rtsp_server_start(rtsp_port);
  if (!engine->rtsp_server) {
    g_warning(
        "RTSP server failed to start on port %d — RTSP streaming disabled",
        rtsp_port);
  } else {
    g_signal_connect(engine->rtsp_server, "client-connected",
                     G_CALLBACK(_on_rtsp_client_connected), engine);
    GstRTSPSessionPool *pool =
        gst_rtsp_server_get_session_pool(engine->rtsp_server);
    g_signal_connect(pool, "session-removed", G_CALLBACK(_on_session_removed),
                     engine);
    g_object_unref(pool);
  }

  /* D-Bus Service */
  engine->dbus_owner_id = dbus_service_start(engine);

  /* NTP Sync Subsystem */
  g_info("[SYS] Starting NTP synchronization daemon...");
  if (sys_ntp_start() != NTP_OK) {
    g_warning("[SYS] NTP daemon failed to start (port in use or thread error)");
  }

  /* D-Bus Proxy to AI Engine */
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (bus) {
    engine->ai_proxy = g_dbus_proxy_new_sync(
        bus, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "com.camera.AIEngine",
        "/com/camera/AIEngine", "com.camera.AIEngine", NULL, &err);
    if (engine->ai_proxy) {
      g_info("Connected to Python AI Engine via D-Bus proxy");
    } else {
      g_warning("Could not create AI Engine proxy (is it running?): %s",
                err ? err->message : "unknown");
      if (err) {
        g_error_free(err);
        err = NULL;
      }
    }
  }

  /* System stats monitor (15s periodic) */
  g_timeout_add_seconds(15, _log_system_stats, engine);

  /* Periodic DOT Dump (120s) */
  g_timeout_add_seconds(120, _dump_all_dots, engine);

  /* Start Robust Scheduler Thread */
  g_mutex_init(&engine->scheduler_mutex);
  g_cond_init(&engine->scheduler_cond);
  engine->scheduler_running = TRUE;
  engine->scheduler_thread =
      g_thread_new("rec-sched", (GThreadFunc)recording_scheduler_loop, engine);

  /* Apply configuration with 3-tier fallback (CLI -> User -> Default) */
  sys_config_load_and_apply(engine);

  /* Final RTSP Mount Point Audit */
  if (engine->rtsp_server) {
    GstRTSPMountPoints *mounts =
        gst_rtsp_server_get_mount_points(engine->rtsp_server);
    if (mounts) {
      g_info("[SYS] Initialized RTSP Server with following mounts:");
      /* We can list them manually by checking our branch_configs keys */
      GHashTableIter m_iter;
      gpointer mk, mv;
      g_hash_table_iter_init(&m_iter, engine->branch_configs);
      while (g_hash_table_iter_next(&m_iter, &mk, &mv)) {
        char mount_path[128];
        snprintf(mount_path, sizeof(mount_path), "/%s", (const char *)mk);
        GstRTSPMediaFactory *mf =
            gst_rtsp_mount_points_match(mounts, mount_path, NULL);
        if (mf) {
          g_info("    ✓ %s (Active Factory)", mount_path);
          g_object_unref(mf);
        } else {
          g_warning("    ✗ %s (FAILED TO MOUNT)", mount_path);
        }
      }
      g_object_unref(mounts);
    }
  }

  /* Signal handlers */
  g_engine = engine;
  signal(SIGINT, _signal_handler);
  signal(SIGTERM, _signal_handler);

  g_info("Engine running. Ctrl+C to stop.");
  g_main_loop_run(engine->loop);

  /* ── Cleanup ── */
  g_info("Shutting down engine...");
  sys_ntp_stop();
  g_bus_unown_name(engine->dbus_owner_id);
  if (engine->ai_proxy)
    g_object_unref(engine->ai_proxy);
  mq_subscriber_free(engine->mq_sub);

  /* Stop all recordings */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, engine->recordings);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    recording_branch_stop((RecordingBranch *)value);
  }

  /* Stop all captures */
  g_hash_table_iter_init(&iter, engine->captures);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    capture_pipeline_stop((CapturePipeline *)value);
  }

  /* Stop scheduler and join thread before destroying hash tables */
  recording_scheduler_stop(engine);

  g_hash_table_destroy(engine->captures);
  g_hash_table_destroy(engine->branch_configs);
  g_hash_table_destroy(engine->recordings);
  g_hash_table_destroy(engine->lenses);
  g_hash_table_destroy(engine->active_branches);
  g_hash_table_destroy(engine->session_mounts);
  g_main_loop_unref(engine->loop);
  g_free(engine);

  g_info("Engine shut down gracefully.");
  return 0;
}
