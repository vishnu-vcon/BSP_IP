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
#include <glib.h>
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

static inline void _compute_effective_cairo_enabled(
    const char *lens_id, const char *tier_name, gboolean overlay_requested,
    gboolean *cairo_active_out, gboolean ntp_overlay_on) {
  (void)lens_id;
  (void)ntp_overlay_on;

  /* Determine if cairooverlay should actually be built into the branch.
   * Third tier always uses a plain software path — no overlay. */
  *cairo_active_out = overlay_requested;

  if (g_strcmp0(tier_name, "third") == 0) {
    *cairo_active_out = FALSE;
  }
}

static inline void _warn_overlay_toggle_limitations(UnifiedEngine *e,
                                               const char *lens_id,
                                               const char *tier_name,
                                               gboolean overlay_on,
                                               CapturePipeline *capture_pipe) {
  (void)e;
  (void)capture_pipe;

  g_debug("[OVERLAY] toggle_overlay '%s/%s': "
          "Setting overlay_enabled=%s on live cairooverlay QData.  "
          "NOTE: The cairooverlay element REMAINS in the pipeline and its "
          "draw callback STILL fires every frame — drawing is just skipped.  "
          "For zero-CPU: rebuild branch with cairo_enabled=FALSE after all "
          "clients disconnect.",
          lens_id, tier_name ? tier_name : "all", overlay_on ? "TRUE" : "FALSE");

  if (!overlay_on) {
    LensInfo *lens_info = g_hash_table_lookup(e->lenses, lens_id);
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

static inline void _audit_overlay_pipeline_elements(UnifiedEngine *e) {
  g_info("[STATS] ══ Overlay & Pipeline Element Audit ══");

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, e->captures);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    CapturePipeline *cap = (CapturePipeline *)value;
    const char *dev = (const char *)key;

    g_info("[STATS] Capture '%s':  cairo_capable=%s  overlay_enabled=%s  "
           "ai_coords=%d  active_branches=%u",
           dev, cap->ai.cairo_capable ? "TRUE" : "FALSE",
           cap->ai.overlay_enabled ? "TRUE" : "FALSE", cap->ai.num_coords,
           g_hash_table_size(cap->dynamic_branches));

    GHashTableIter br_iter;
    gpointer bk, bv;
    g_mutex_lock(&cap->branch_mutex);
    g_hash_table_iter_init(&br_iter, cap->dynamic_branches);
    while (g_hash_table_iter_next(&br_iter, &bk, &bv)) {
      const char *bname = (const char *)bk;
      DynamicBranch *dyn = (DynamicBranch *)bv;
      if (!dyn->elements)
        continue;

      for (int i = 0; i < dyn->num_elements; i++) {
        if (!dyn->elements[i])
          continue;
        GstElementFactory *f = gst_element_get_factory(dyn->elements[i]);
        if (!f)
          continue;
        const char *fname = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f));

        if (g_strcmp0(fname, "cairooverlay") == 0) {
          gboolean oe = GPOINTER_TO_INT(
              g_object_get_data(G_OBJECT(dyn->elements[i]), "overlay_enabled"));
          int bw = GPOINTER_TO_INT(
              g_object_get_data(G_OBJECT(dyn->elements[i]), "branch_w"));
          int bh = GPOINTER_TO_INT(
              g_object_get_data(G_OBJECT(dyn->elements[i]), "branch_h"));
          g_info("[STATS]   branch='%s'  cairooverlay: overlay_enabled=%s  "
                 "branch_res=%dx%d  "
                 "(NOTE: draw callback fires every frame regardless of enabled "
                 "flag)",
                 bname, oe ? "TRUE" : "FALSE", bw, bh);
        }

        if (g_strcmp0(fname, "clockoverlay") == 0) {
          gboolean silent = FALSE;
          g_object_get(dyn->elements[i], "silent", &silent, NULL);
          g_info("[STATS]   branch='%s'  clockoverlay: silent=%s  "
                 "(NOTE: silent=TRUE still runs full render path each frame)",
                 bname, silent ? "TRUE" : "FALSE");
        }
      }
    }
    g_mutex_unlock(&cap->branch_mutex);
  }
}

static UnifiedEngine *g_engine = NULL;

/* ── Lens helpers (port of _get_capture, is_overlay_enabled) ── */

static CapturePipeline *_get_capture(UnifiedEngine *e, const char *lens) {
  LensInfo *info = g_hash_table_lookup(e->lenses, lens);
  if (!info)
    return NULL;
  return g_hash_table_lookup(e->captures, info->device);
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

static void _save_user_config_unlocked(UnifiedEngine *e);
char *engine_update_stream(UnifiedEngine *e, const char *lens, const char *tier,
                           const char *json);
gboolean stream_manager_is_active(StreamManager *mgr, const char *lens,
                                  const char *tier);
void stream_manager_force_teardown(StreamManager *mgr, const char *lens,
                                   const char *tier);

/* ── Global API to trigger a config sync to disk (non-locking version for
 * scheduler) ── */
void engine_persist_state(UnifiedEngine *e) { _save_user_config_unlocked(e); }

/* ── Public API (locking version for D-Bus/API) ── */
void engine_sync_config(UnifiedEngine *e) {
  g_mutex_lock(&e->config_mutex);
  _save_user_config_unlocked(e);
  g_mutex_unlock(&e->config_mutex);
}

static void _save_user_config_unlocked(UnifiedEngine *e) {
  if (e->config_path[0] == '\0')
    return;

  gchar *contents = NULL;
  gsize len;
  if (!g_file_get_contents(e->config_path, &contents, &len, NULL)) {
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
  gpointer lk, lv;
  g_hash_table_iter_init(&lu_iter, e->lenses);
  while (g_hash_table_iter_next(&lu_iter, &lk, &lv)) {
    const char *lid = (const char *)lk;
    LensInfo *info = (LensInfo *)lv;

    JsonObject *lens_obj = NULL;
    if (json_object_has_member(overrides, lid)) {
      lens_obj = json_object_get_object_member(overrides, lid);
    }
    if (!lens_obj) {
      lens_obj = json_object_new();
      json_object_set_object_member(overrides, lid, lens_obj);
    }

    /* [FIX-1] Persist BOTH the hardware capability flag ("cairo") AND the
     * user-visible software render state ("overlay").  Previously only
     * cairo_capable was written, so a user who had disabled the overlay via
     * ToggleOverlay or ConfigureLens would find it silently re-enabled on
     * every reboot because overlay_enabled was never saved to disk.
     * sys_config.c already forwards both keys into engine_configure_lens,
     * and engine_configure_lens reads them at the cairo/overlay branches, so
     * the restore path is complete once the save side is correct. */
    json_object_set_boolean_member(lens_obj, "cairo", info->cairo_capable);
    json_object_set_boolean_member(lens_obj, "overlay", info->overlay_enabled);

    /* Persist AI thresholds */
    JsonObject *ai_obj = json_object_new();
    json_object_set_double_member(ai_obj, "ai_threshold", info->ai_threshold);
    json_object_set_double_member(ai_obj, "snapshot_threshold", info->snapshot_threshold);
    json_object_set_object_member(lens_obj, "ai", ai_obj);
    json_object_set_boolean_member(lens_obj, "overlay", info->overlay_enabled);

    /* ───── SYNC BRANCHES ───── */
    GHashTableIter br_iter;
    gpointer bk, bv;
    g_hash_table_iter_init(&br_iter, e->branch_configs);
    while (g_hash_table_iter_next(&br_iter, &bk, &bv)) {
      const char *branch_key = (const char *)bk;
      BranchConfig *cfg = (BranchConfig *)bv;
      if (!g_str_has_prefix(branch_key, lid))
        continue;

      const char *tier_name = strrchr(branch_key, '/');
      if (tier_name)
        tier_name++;
      else
        tier_name = branch_key;

      JsonObject *tier_obj = json_object_new();
      json_object_set_string_member(tier_obj, "resolution", cfg->resolution);
      json_object_set_int_member(tier_obj, "fps", cfg->fps);
      json_object_set_string_member(tier_obj, "encoder", cfg->codec);
      json_object_set_boolean_member(tier_obj, "overlay", cfg->overlay_enabled);
      json_object_set_boolean_member(tier_obj, "ntp_overlay", cfg->ntp_overlay);
      json_object_set_object_member(lens_obj, tier_name, tier_obj);
    }

    /* ───── SYNC RECORDING DEFAULTS ───── */
    if (info->rec_defaults) {
      JsonObject *rec_node = json_object_new();
      json_object_set_string_member(rec_node, "resolution", info->rec_defaults->resolution);
      json_object_set_int_member(rec_node, "fps", info->rec_defaults->fps);
      json_object_set_string_member(rec_node, "encoder", info->rec_defaults->codec);
      json_object_set_int_member(rec_node, "bitrate", info->rec_defaults->bitrate);
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
    snprintf(bak_path, sizeof(bak_path), "%s.bak", e->config_path);

    if (g_file_test(e->config_path, G_FILE_TEST_EXISTS)) {
      /* Ignore rename errors — best-effort; if it fails the old .bak (if any)
       * remains, which is still better than nothing. */
      if (g_rename(e->config_path, bak_path) != 0) {
        g_debug("[CONFIG] .bak rotation skipped (rename errno=%d) — continuing "
                "save",
                errno);
      }
    }

    if (!g_file_set_contents(e->config_path, new_json, -1, &save_err)) {
      g_warning("[CONFIG] SAVE FAILED for '%s': %s  "
                "(last known-good config preserved at '%s')",
                e->config_path, save_err ? save_err->message : "unknown error",
                bak_path);
      if (save_err)
        g_error_free(save_err);
    } else {
      g_debug("[CONFIG] Saved atomically to '%s'  (backup: '%s')",
              e->config_path, bak_path);
    }
    g_free(new_json);
  }
  g_object_unref(gen);
  g_object_unref(parser);
}

static void _save_user_config(UnifiedEngine *e) {
  g_mutex_lock(&e->config_mutex);
  _save_user_config_unlocked(e);
  g_mutex_unlock(&e->config_mutex);
}

/* ══════════════════════════════════════════════════════════════
 *  D-Bus Method Implementations
 * ══════════════════════════════════════════════════════════════ */

/* ── ConfigureLens ── */
char *engine_configure_lens(UnifiedEngine *e, const char *json) {
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
  LensInfo *lens_info = g_hash_table_lookup(e->lenses, lens);
  if (!lens_info) {
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Unknown lens: %s\"}", lens);
  }

  gboolean previous_cairo_capable = lens_info->cairo_capable;

  if (!lens_info->ai_threshold) lens_info->ai_threshold = 0.4;
  if (!lens_info->snapshot_threshold) lens_info->snapshot_threshold = 0.4;

  if (json_object_has_member(obj, "ai")) {
    JsonObject *ai_obj = json_object_get_object_member(obj, "ai");
    if (json_object_has_member(ai_obj, "ai_threshold"))
      lens_info->ai_threshold = json_object_get_double_member(ai_obj, "ai_threshold");
    if (json_object_has_member(ai_obj, "snapshot_threshold"))
      lens_info->snapshot_threshold = json_object_get_double_member(ai_obj, "snapshot_threshold");
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
   * If the global hardware capability changed (e.g. Cairo disabled for the
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
    g_hash_table_iter_init(&iter, e->branch_configs);
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
            stream_manager_force_teardown(e->stream_mgr, lens, tier);
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
   * change (e.g. FPS or resolution), discarding a previously-set disable.
   * Fix: only update user_overlay_on when the key is explicitly present. */
  if (json_object_has_member(obj, "overlay")) {
    lens_info->user_overlay_on = json_object_get_boolean_member(obj, "overlay");
  }
  /* else: preserve the stored_cfg user preference unchanged */

  /* Ensure capture running */
  const char *device = lens_info->device;
  if (!g_hash_table_contains(e->captures, device)) {
    CapturePipeline *capture_pipe =
        capture_pipeline_new(device, e->default_caps, lens_info->cairo_capable);
    capture_pipeline_start(capture_pipe);
    g_hash_table_insert(e->captures, g_strdup(device), capture_pipe);
  }
  CapturePipeline *capture_pipe = g_hash_table_lookup(e->captures, device);
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
  for (GList *m = members; m != NULL; m = m->next) {
    const char *tier = (const char *)m->data;
    JsonNode *tier_node = json_object_get_member(obj, tier);
    if (!tier_node || !JSON_NODE_HOLDS_OBJECT(tier_node)) continue;
    
    JsonObject *tier_obj = json_node_get_object(tier_node);
    const char *tier_state = "created";

    /* Handle recording defaults */
    if (g_strcmp0(tier, "recording") == 0) {
      if (!lens_info->rec_defaults) lens_info->rec_defaults = g_new0(BranchConfig, 1);
      
      const char *r_res = json_object_has_member(tier_obj, "resolution") 
          ? json_object_get_string_member(tier_obj, "resolution") : "1920x1080";
      g_strlcpy(lens_info->rec_defaults->resolution, r_res, sizeof(lens_info->rec_defaults->resolution));
      sscanf(r_res, "%dx%d", &lens_info->rec_defaults->w, &lens_info->rec_defaults->h);
      
      lens_info->rec_defaults->fps = json_object_has_member(tier_obj, "fps")
          ? (int)json_object_get_int_member(tier_obj, "fps") : 25;
          
      g_strlcpy(lens_info->rec_defaults->codec, json_object_has_member(tier_obj, "encoder")
          ? json_object_get_string_member(tier_obj, "encoder") : "h264", 
          sizeof(lens_info->rec_defaults->codec));
          
      lens_info->rec_defaults->bitrate = json_object_has_member(tier_obj, "bitrate")
          ? (int)json_object_get_int_member(tier_obj, "bitrate") : 4000000;
          
      tier_state = "configured_defaults";
    }

    if (g_strcmp0(tier, "lens") == 0 || g_strcmp0(tier, "cairo") == 0 ||
        g_strcmp0(tier, "overlay") == 0 || g_strcmp0(tier, "recording") == 0)
      continue;

    char branch_key[128], mount_path[128];
    snprintf(branch_key, sizeof(key), "%s/%s", lens, tier);
    snprintf(mount_path, sizeof(mount_path), "/%s/%s", lens, tier);

    BranchConfig *stored_cfg = g_hash_table_lookup(e->branch_configs, branch_key);

    /* ── FIELD COALESCING: Use stored_cfg values if JSON fields are missing ── */
    /* [STRICT-THIRD] Force 480p @ 3 FPS for third tier, ignore JSON/Existing */
    const char *resolution_str = (g_strcmp0(tier, "third") == 0) ? "640x480" :
        (json_object_has_member(tier_obj, "resolution")
            ? json_object_get_string_member(tier_obj, "resolution")
            : (stored_cfg ? stored_cfg->resolution : "1920x1080"));

    int target_fps = (g_strcmp0(tier, "third") == 0) ? 3 :
        (json_object_has_member(tier_obj, "fps")
            ? (int)json_object_get_int_member(tier_obj, "fps")
            : (stored_cfg ? stored_cfg->fps : 25));

    const char *codec_name = (g_strcmp0(tier, "third") == 0) ? "mjpeg" :
        (json_object_has_member(tier_obj, "encoder")
            ? json_object_get_string_member(tier_obj, "encoder")
            : (stored_cfg ? stored_cfg->codec : "h264"));

    gboolean user_overlay_on =
        json_object_has_member(tier_obj, "overlay")
            ? json_object_get_boolean_member(tier_obj, "overlay")
            : (stored_cfg ? stored_cfg->user_overlay_on : TRUE);

    gboolean ntp_on =
        json_object_has_member(tier_obj, "ntp_on")
            ? json_object_get_boolean_member(tier_obj, "ntp_on")
            : (stored_cfg ? stored_cfg->ntp_on : FALSE);

    /* ── SCENARIO CHECK ── */
    gboolean stream_is_live = stream_manager_is_active(e->stream_mgr, lens, tier);

    if (stored_cfg) {
      /* Compute the EFFECTIVE cairo state for the old and new configurations.
       * These fold in lens_info->cairo_capable so that disabling cairo at the lens
       * level is correctly detected as a structural change even when the raw
       * user_overlay_on flag is the same. */
      gboolean was_cairo_active;
      _compute_effective_cairo_enabled(
          lens, tier, lens_info->cairo_capable && stored_cfg->user_overlay_on,
          &was_cairo_active, stored_cfg->ntp_on);

      gboolean will_cairo_be_active;
      _compute_effective_cairo_enabled(lens, tier,
                                        lens_info->cairo_capable && user_overlay_on,
                                        &will_cairo_be_active, ntp_on);

      /* A structural rebuild is required when codec, effective cairo state, or
       * NTP overlay changes — these alter which GStreamer elements are in the
       * branch and cannot be applied dynamically.
       * Resolution and FPS are dynamic-update safe (videorate / capsfilter). */
      gboolean codec_unchanged = (g_strcmp0(stored_cfg->codec, codec_name) == 0);

      /* BUG FIX [FIX-1]: was_cairo_active / will_cairo_be_active are used here
       * instead of the raw user_overlay_on flags.  The previous code compared
       * stored_cfg->user_overlay_on == user_overlay_on, which ignored changes to
       * lens_info->cairo_capable.  Disabling cairo at the lens level would leave
       * user_overlay_on unchanged, so overlay_match returned TRUE and the
       * cairooverlay element was never stripped from the branch. */
      gboolean pipeline_structure_matches = (was_cairo_active == will_cairo_be_active &&
                                             stored_cfg->ntp_on == ntp_on);

      if (codec_unchanged && pipeline_structure_matches) {
        /* ───── DYNAMIC UPDATE ───── */
        stored_cfg->fps = target_fps;
        sscanf(resolution_str, "%dx%d", &stored_cfg->w, &stored_cfg->h);
        g_strlcpy(stored_cfg->resolution, resolution_str, sizeof(stored_cfg->resolution));
        stored_cfg->user_overlay_on = user_overlay_on;
        stored_cfg->ntp_on = ntp_on;

        if (stream_is_live) {
          g_info("[CFG] Active path: Propagating dynamic params to '%s/%s'",
                 lens, tier);
          char *dynamic_update_json =
              g_strdup_printf("{\"resolution\":\"%s\",\"fps\":%d,\"overlay\":%"
                              "s,\"ntp_on\":%s}",
                              resolution_str, target_fps, user_overlay_on ? "true" : "false",
                              ntp_on ? "true" : "false");
          engine_update_stream(e, lens, tier, dynamic_update_json);
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
            gst_rtsp_server_get_mount_points(e->rtsp_server);
        gst_rtsp_mount_points_remove_factory(rtsp_mounts, mount_path);
        g_object_unref(rtsp_mounts);

        /* Remove the live GStreamer branch immediately so the new pipeline
         * is ready for the next client connect, without waiting for all
         * current clients to voluntarily disconnect. */
        if (stream_is_live) {
          const char *live_branch_name =
              stream_manager_get_active_branch_name(e->stream_mgr, lens, tier);
          if (live_branch_name && capture_pipe) {
            g_info("[CFG]   Force-removing live branch '%s' from pipeline",
                   live_branch_name);
            capture_pipeline_remove_branch(capture_pipe, live_branch_name);
          }
        }

        stream_manager_force_teardown(e->stream_mgr, lens, tier);

        g_hash_table_remove(e->branch_configs, branch_key);
        stored_cfg = NULL;
        tier_state = "rebuilt";
      }
    }

    if (!stored_cfg) {
      BranchConfig *new_branch_cfg = branch_config_new(resolution_str, target_fps, codec_name);
      cfg->ntp_on = ntp_on;
      cfg->user_overlay_on = user_overlay_on;
      g_hash_table_insert(e->branch_configs, g_strdup(key), cfg);

      /* Exclude cairo from build if hardware disabled OR user disabled */
      gboolean build_cairo;
      _compute_effective_cairo_enabled(lens, tier,
                                        lens_info->cairo_capable && user_overlay_on,
                                        &build_cairo, ntp_on);

      /* Mount RTSP rtsp_factory */
      GstRTSPMountPoints *rtsp_mounts =
          gst_rtsp_server_get_mount_points(e->rtsp_server);
      GstRTSPMediaFactory *rtsp_factory = lazy_rtsp_factory_create(
          e, capture_pipe, e->stream_mgr, lens, tier, new_branch_cfg, mount_path, build_cairo);
      gst_rtsp_mount_points_add_factory(rtsp_mounts, mount_path, rtsp_factory);
      g_object_unref(rtsp_mounts);
    }

    g_info("Configured: rtsp://0.0.0.0:%d%s (%s %dfps %s) [%s]", e->rtsp_port,
           mount, resolution_str, target_fps, codec_name, tier_state);

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
  if (!g_hash_table_contains(e->branch_configs, third_key)) {
    g_info("[CFG]   Auto-mounting mandatory 'third' branch for %s", lens);
    
    BranchConfig *new_branch_cfg = branch_config_new("640x480", 3, "mjpeg");
    cfg->ntp_on = FALSE;
    cfg->user_overlay_on = FALSE;
    g_hash_table_insert(e->branch_configs, g_strdup(third_key), cfg);

    char mount[128];
    snprintf(mount, sizeof(mount), "/%s/third", lens);
    
    GstRTSPMountPoints *rtsp_mounts = gst_rtsp_server_get_mount_points(e->rtsp_server);
    GstRTSPMediaFactory *rtsp_factory = lazy_rtsp_factory_create(
        e, capture_pipe, e->stream_mgr, lens, "third", new_branch_cfg, mount_path, FALSE);
    gst_rtsp_mount_points_add_factory(rtsp_mounts, mount_path, rtsp_factory);
    g_object_unref(rtsp_mounts);
    
    g_info("Configured: rtsp://0.0.0.0:%d%s (640x480 3fps mjpeg) [auto-start]", 
           e->rtsp_port, mount);
  }

  json_builder_end_object(response_builder); /* branches */
  json_builder_end_object(response_builder); /* root */

  _save_user_config_unlocked(e);

  JsonGenerator *result_gen = json_generator_new();
  json_generator_set_root(result_gen, json_builder_get_root(response_builder));
  char *result = json_generator_to_data(result_gen, NULL);
  g_object_unref(result_gen);
  g_object_unref(response_builder);
  g_object_unref(parser);
  return result;
}

/* ── UpdateStreamParams / _apply_dynamic_update ── */
char *engine_update_stream(UnifiedEngine *e, const char *lens,
                           const char *branch, const char *json) {
  char branch_key[128];
  snprintf(branch_key, sizeof(branch_key), "%s/%s", lens, branch);
  BranchConfig *branch_cfg = g_hash_table_lookup(e->branch_configs, branch_key);
  if (!branch_cfg) {
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Branch '%s' not configured\"}",
        key);
  }

  CapturePipeline *capture_pipe = _get_capture(e, lens);
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
      stream_manager_get_active_branch_name(e->stream_mgr, lens, branch);
  DynamicBranch *active_branch = NULL;

  if (active_branch_name) {
    active_branch = g_hash_table_lookup(capture_pipe->dynamic_branches, active_branch_name);
  } else {
    /* Legacy fallback for non-shared branches (if any) */
    char legacy_branch_name[64];
    snprintf(legacy_branch_name, sizeof(legacy_branch_name), "rtsp_%s_%s", lens, branch);
    active_branch = g_hash_table_lookup(capture_pipe->dynamic_branches, legacy_branch_name);
  }

  /* NEVER fall back to rec_ branches here!
     Dynamic stream updates on active MP4 recordings corrupt the muxer container
     headers. */

  if (!active_branch || !active_branch->elements) {
    g_mutex_unlock(&capture_pipe->branch_mutex);
    /* Branch idle — just save config for next connect */
    if (json_object_has_member(update_params, "fps"))
      branch_cfg->fps = (int)json_object_get_int_member(update_params, "fps");
    if (json_object_has_member(update_params, "resolution")) {
      const char *res = json_object_get_string_member(update_params, "resolution");
      g_strlcpy(branch_cfg->resolution, res, sizeof(branch_cfg->resolution));
      sscanf(res, "%dx%d", &branch_cfg->w, &branch_cfg->h);
    }
    if (json_object_has_member(update_params, "ntp_overlay"))
      branch_cfg->ntp_overlay = json_object_get_boolean_member(update_params, "ntp_overlay");
    if (json_object_has_member(update_params, "overlay"))
      branch_cfg->overlay_enabled = json_object_get_boolean_member(update_params, "overlay");
    _save_user_config(e);
    g_object_unref(json_parser);
    return g_strdup("{\"status\":\"config_updated\",\"note\":\"Branch idle, "
                    "config saved for next connect\"}");
  }

  /* Apply Cairo Overlay toggle dynamically */
  if (json_object_has_member(update_params, "overlay")) {
    gboolean overlay_on = json_object_get_boolean_member(update_params, "overlay");
    gboolean cairo_elem_found = FALSE;
    for (int i = 0; i < active_branch->num_elements; i++) {
      const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
      if (g_str_has_prefix(name, "cairo_")) {
        g_object_set_data(G_OBJECT(active_branch->elements[i]), "overlay_enabled",
                          GINT_TO_POINTER(overlay_on));
        cairo_elem_found = TRUE;
        any_param_changed = TRUE;
        g_info("Dynamic update: %s overlay → %s", key,
               overlay_on ? "ON" : "OFF");
      }
    }
    branch_cfg->overlay_enabled = overlay_on;
    if (!cairo_elem_found) {
      g_info("Dynamic update: %s overlay config saved (%s). "
             "Element not in pipeline — reconfigure lens to include it.",
             key, overlay_on ? "ON" : "OFF");
      any_param_changed = TRUE;
    }
  }

  /* Apply NTP overlay change (clockoverlay) */
  if (json_object_has_member(update_params, "ntp_overlay")) {
    gboolean ntp_on =
        json_object_get_boolean_member(update_params, "ntp_overlay");
    gboolean clock_elem_found = FALSE;
    for (int i = 0; i < active_branch->num_elements; i++) {
      GstElementFactory *f = gst_element_get_factory(active_branch->elements[i]);
      if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)),
                         "clockoverlay") == 0) {
        g_object_set(active_branch->elements[i], "silent", !ntp_on, NULL);
        clock_elem_found = TRUE;
        any_param_changed = TRUE;
        g_info("Dynamic update: %s ntp_overlay → %s", key,
               ntp_on ? "ON" : "OFF");
      }
    }
    /* Always save the config — if clockoverlay wasn't in the pipeline
     * (because it was excluded at build time), the change takes effect
     * on next stream connect when the branch is rebuilt. */
    branch_cfg->ntp_overlay = ntp_on;
    if (!clock_elem_found) {
      g_info("Dynamic update: %s ntp_overlay config saved (%s). "
             "Takes effect on next stream connect.",
             key, ntp_on ? "ON" : "OFF");
      any_param_changed = TRUE;
    }
  }

  /* Apply FPS change to videorate element AND fps capsfilter */
  if (json_object_has_member(update_params, "fps") && g_strcmp0(branch, "third") != 0) {
    int fps = (int)json_object_get_int_member(update_params, "fps");
    /* [STABILITY] Clamp FPS to global maximum of 30. */
    int effective_fps = (fps > 30) ? 30 : ((fps > 0) ? fps : 30);
    for (int i = 0; i < active_branch->num_elements; i++) {
      const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
      if (g_str_has_prefix(name, "rate_")) {
        g_object_set(active_branch->elements[i], "max-rate", effective_fps, NULL);
        branch_cfg->fps = effective_fps;
        any_param_changed = TRUE;
        g_info("Dynamic update: %s fps → %d (videorate)", key, effective_fps);
      }
      if (g_str_has_prefix(name, "fps_caps_")) {
        char fps_str[128];
        snprintf(fps_str, sizeof(fps_str), "video/x-raw,framerate=%d/1",
                 effective_fps);
        GstCaps *new_fps_caps = gst_caps_from_string(fps_str);
        g_object_set(active_branch->elements[i], "caps", new_fps_caps, NULL);
        gst_caps_unref(new_fps_caps);
        g_info("Dynamic update: %s fps → %d (capsfilter)", key, effective_fps);
      }
    }
  }

  /* Apply resolution change to capsfilter + update cairooverlay dimensions */
  if (json_object_has_member(update_params, "resolution") && g_strcmp0(branch, "third") != 0) {
    const char *res = json_object_get_string_member(update_params, "resolution");
    int w, h;
    sscanf(res, "%dx%d", &w, &h);
    for (int i = 0; i < active_branch->num_elements; i++) {
      const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
      if (g_str_has_prefix(name, "res_")) {
        char caps_str[256];
        snprintf(
            caps_str, sizeof(caps_str),
            "video/x-raw,width=%d,height=%d",
            w, h);
        GstCaps *new_caps = gst_caps_from_string(caps_str);
        g_object_set(active_branch->elements[i], "caps", new_caps, NULL);
        gst_caps_unref(new_caps);
        g_strlcpy(branch_cfg->resolution, res, sizeof(branch_cfg->resolution));
        branch_cfg->w = w;
        branch_cfg->h = h;
        any_param_changed = TRUE;
        g_info("Dynamic update: %s resolution → %s", key, res);
      }
      /* Also update cairooverlay branch dimensions so bounding boxes scale
       * correctly */
      if (g_str_has_prefix(name, "cairo_")) {
        g_object_set_data(G_OBJECT(active_branch->elements[i]), "branch_w",
                          GINT_TO_POINTER(w));
        g_object_set_data(G_OBJECT(active_branch->elements[i]), "branch_h",
                          GINT_TO_POINTER(h));
        g_info("Dynamic update: %s cairooverlay dimensions → %dx%d", key, w, h);
      }
    }
  }

  /* Apply bitrate change to encoder */
  if (json_object_has_member(update_params, "bitrate")) {
    int bitrate = (int)json_object_get_int_member(update_params, "bitrate");
    for (int i = 0; i < active_branch->num_elements; i++) {
      const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
      if (g_str_has_prefix(name, "enc_")) {
        GstElementFactory *elem_factory = gst_element_get_factory(active_branch->elements[i]);
        const char *fname =
            elem_factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(elem_factory))
                    : "";
        if (strstr(fname, "v4l2")) {
          char ctrl[256];
          snprintf(ctrl, sizeof(ctrl),
                   "controls,video_bitrate=%d,video_gop_size=%d", bitrate,
                   branch_cfg->fps > 0 ? branch_cfg->fps : 30);
          GstStructure *ec = gst_structure_new_from_string(ctrl);
          if (ec) {
            g_object_set(active_branch->elements[i], "extra-controls", ec, NULL);
            gst_structure_free(ec);
            any_param_changed = TRUE;
          }
        } else if (strstr(fname, "x264")) {
          g_object_set(active_branch->elements[i], "bitrate", bitrate / 1000, NULL);
          any_param_changed = TRUE;
        }
        g_info("Dynamic update: %s bitrate → %d", key, bitrate);
        break;
      }
    }
  }

  /* Apply bitrate mode change (VBR/CBR) to encoder */
  if (json_object_has_member(update_params, "bitrate_mode")) {
    const char *mode = json_object_get_string_member(update_params, "bitrate_mode");
    for (int i = 0; i < active_branch->num_elements; i++) {
      const char *name = GST_ELEMENT_NAME(active_branch->elements[i]);
      if (g_str_has_prefix(name, "enc_")) {
        GstElementFactory *elem_factory = gst_element_get_factory(active_branch->elements[i]);
        const char *fname =
            elem_factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(elem_factory))
                    : "";
        if (strstr(fname, "v4l2")) {
          /* VPU hw encoder NXP patch: "vbr-mode" property. 1=VBR, 0=CBR */
          int vbm = (g_strcmp0(mode, "vbr") == 0) ? 1 : 0;
          if (g_object_class_find_property(G_OBJECT_GET_CLASS(active_branch->elements[i]),
                                           "vbr-mode")) {
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
        g_info("Dynamic update: %s bitrate_mode → %s", key, mode);
        break;
      }
    }
  }

  g_mutex_unlock(&capture_pipe->branch_mutex);

  char *response =
      any_param_changed
          ? g_strdup_printf(
                "{\"status\":\"any_param_changed\",\"lens\":\"%s\",\"branch\":\"%s\"}",
                lens, branch)
          : g_strdup("{\"status\":\"no_change\"}");

  if (any_param_changed)
    _save_user_config(e);

  g_object_unref(json_parser);
  return response;
}

/* ── StopLens (full cascade teardown) ── */
char *engine_stop_lens(UnifiedEngine *e, const char *json) {
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
  LensInfo *info = g_hash_table_lookup(e->lenses, lens);
  if (!info) {
    g_object_unref(parser);
    return g_strdup("{\"status\":\"error\",\"message\":\"Unknown lens\"}");
  }

  /* Stop recordings for this lens */
  GHashTableIter iter;
  gpointer k, v;
  GList *keys_to_remove = NULL;
  g_hash_table_iter_init(&iter, e->recordings);
  while (g_hash_table_iter_next(&iter, &k, &v)) {
    if (g_str_has_prefix((const char *)k, lens)) {
      /* Destructor in hash table handles stopping */
      keys_to_remove = g_list_append(keys_to_remove, g_strdup((const char *)k));
    }
  }
  for (GList *l = keys_to_remove; l; l = l->next) {
    g_hash_table_remove(e->recordings, l->data);
    g_free(l->data);
  }
  g_list_free(keys_to_remove);

  /* Remove RTSP factories for this lens */
  GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(e->rtsp_server);
  keys_to_remove = NULL;
  g_hash_table_iter_init(&iter, e->branch_configs);
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
    g_hash_table_remove(e->branch_configs, l->data);
    g_free(l->data);
  }
  g_list_free(keys_to_remove);

  /* Stop capture pipeline */
  CapturePipeline *cap = g_hash_table_lookup(e->captures, info->device);
  if (cap) {
    capture_pipeline_stop(cap);
    g_hash_table_remove(e->captures, info->device);
  }

  char *resp =
      g_strdup_printf("{\"status\":\"stopped\",\"lens\":\"%s\"}", lens);
  g_object_unref(parser);
  return resp;
}

/* ── TakeSnapshot ── */
char *engine_take_snapshot(UnifiedEngine *e, const char *lens,
                           const char *path) {
  if (!lens || !path || path[0] == '\0')
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"lens and path required\"}");

  CapturePipeline *cap = _get_capture(e, lens);
  if (!cap)
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"No capture for %s\"}", lens);

  GstSample *sample = capture_pipeline_grab_snapshot(cap);
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
_parse_recording_branch_config(UnifiedEngine *e, const char *lens,
                               const char *tier, const char *mode,
                               JsonObject *obj, char **err_msg) {
  BranchConfig *rec_cfg = g_new0(BranchConfig, 1);

  LensInfo *info = g_hash_table_lookup(e->lenses, lens);

  if (g_strcmp0(mode, "event") == 0 || g_strcmp0(mode, "scheduled") == 0) {
    if (info && info->rec_defaults) {
        *rec_cfg = *(info->rec_defaults);
    } else if (g_strcmp0(mode, "event") == 0) {
        /* Legacy fallback for AI if no defaults in config */
        rec_cfg->w = 1280;
        rec_cfg->h = 720;
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
    char key[128];
    snprintf(key, sizeof(key), "%s/%s", lens, tier);
    BranchConfig *cfg = g_hash_table_lookup(e->branch_configs, key);
    if (!cfg) {
      g_free(rec_cfg);
      *err_msg = g_strdup_printf(
          "{\"status\":\"error\",\"message\":\"Branch '%s' not configured\"}",
          key);
      return NULL;
    }
    *rec_cfg = *cfg; /* Copy defaults */
  }
  if (json_object_has_member(obj, "resolution")) {
    const char *res = json_object_get_string_member(obj, "resolution");
    sscanf(res, "%dx%d", &rec_cfg->w, &rec_cfg->h);
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
static char *_check_recording_locks(UnifiedEngine *e, const char *lens,
                                    const char *tier,
                                    RecordingBranch *new_rec) {
  GHashTableIter iter;
  gpointer rk, rv;
  g_hash_table_iter_init(&iter, e->recordings);

  while (g_hash_table_iter_next(&iter, &rk, &rv)) {
    RecordingBranch *existing = (RecordingBranch *)rv;
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
char *engine_start_recording(UnifiedEngine *e, const char *json) {
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
      _parse_recording_branch_config(e, lens, tier, mode, obj, &cfg_err);
  if (!rec_cfg) {
    g_object_unref(parser);
    return cfg_err;
  }

  /* 2. System Hardware Validation (Pre-requisite for branch creation) */
  CapturePipeline *cap = _get_capture(e, lens);
  if (!cap) {
    g_free(rec_cfg);
    g_object_unref(parser);
    return g_strdup("{\"status\":\"error\",\"message\":\"No capture node "
                    "active for target.\"}");
  }

  /* 3. Create Branch Object (Used for overlap checking) */
  RecordingBranch *rec = recording_branch_new(e, cap, lens, tier, dir, rec_cfg,
                                              mode, idle_timeout, max_seg);
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
  g_mutex_lock(&e->scheduler_mutex);
  char *lock_err = _check_recording_locks(e, lens, tier, rec);
  if (lock_err) {
    g_mutex_unlock(&e->scheduler_mutex);
    recording_branch_free(rec);
    g_object_unref(parser);
    return lock_err;
  }

  /* 5. Start Recording */
  if (recording_branch_start(rec)) {
    g_hash_table_insert(e->recordings, g_strdup(key), rec);

    /* Nudge the scheduler thread to re-evaluate the next event */
    g_cond_signal(&e->scheduler_cond);
    g_mutex_unlock(&e->scheduler_mutex);

    char *resp = g_strdup_printf(
        "{\"status\":\"recording\",\"lens\":\"%s\",\"branch\":\"%s\"}", lens,
        tier);

    _save_user_config(e);
    g_object_unref(parser);
    return resp;
  }
  g_mutex_unlock(&e->scheduler_mutex);

  char *err_resp = g_strdup_printf(
      "{\"status\":\"error\",\"message\":\"Failed to start recording %s\"}",
      key);
  g_object_unref(parser);
  recording_branch_free(rec);
  return err_resp;
}

/* ── StopRecording ── */
char *engine_stop_recording(UnifiedEngine *e, const char *json) {
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

  g_mutex_lock(&e->scheduler_mutex);
  RecordingBranch *rec = g_hash_table_lookup(e->recordings, key);
  if (!rec) {
    g_mutex_unlock(&e->scheduler_mutex);
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"No recording for %s\"}", key);
  }

  g_hash_table_remove(e->recordings, key);

  /* Nudge scheduler to re-calculate now that a slot is free */
  g_cond_signal(&e->scheduler_cond);
  g_mutex_unlock(&e->scheduler_mutex);

  g_object_unref(parser);
  return g_strdup("{\"status\":\"stopped\"}");
}

/* ── StartHLS ── */
char *engine_start_hls(UnifiedEngine *e, const char *json) {
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

  if (g_hash_table_contains(e->hls_generators, key)) {
    HLSGenerator *existing_hls = g_hash_table_lookup(e->hls_generators, key);
    hls_generator_refresh(existing_hls);
    g_object_unref(parser);
    return g_strdup_printf(
        "{\"status\":\"active\",\"message\":\"HLS refreshed for %s\"}", key);
  }

  /* Ensure capture exists */
  CapturePipeline *cap = _get_capture(e, lens);
  if (!cap) {
    g_object_unref(parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"No capture for lens\"}");
  }

  /* Get branch config */
  BranchConfig *cfg = g_hash_table_lookup(e->branch_configs, key);
  if (!cfg) {
    g_object_unref(parser);
    return g_strdup(
        "{\"status\":\"error\",\"message\":\"Branch not configured\"}");
  }

  /* Look up lens info for Cairo capability and combine with branch config */
  LensInfo *info = g_hash_table_lookup(e->lenses, lens);
  gboolean cairo_enabled;
  _compute_effective_cairo_enabled(
      lens, tier, (info ? info->cairo_capable : TRUE) && cfg->overlay_enabled,
      &cairo_enabled, cfg->ntp_overlay);

  /* We need to ACQUIRE the encoder before starting HLS generator */
  if (!stream_manager_acquire(e->stream_mgr, cap, lens, tier, cfg,
                              cairo_enabled)) {
    g_object_unref(parser);
    return g_strdup("{\"status\":\"error\",\"message\":\"Failed to acquire "
                    "shared encoder\"}");
  }

  HLSGenerator *hls =
      hls_generator_new(e->stream_mgr, lens, tier, cfg->codec, dir);
  hls_generator_start(hls);
  g_hash_table_insert(e->hls_generators, g_strdup(key), hls);

  g_info("[HLS] Started HLS for %s at %s", key, dir);
  char *resp = g_strdup_printf(
      "{\"status\":\"started\",\"lens\":\"%s\",\"branch\":\"%s\"}", lens, tier);
  g_object_unref(parser);
  return resp;
}

/* ── StopHLS ── */
char *engine_stop_hls(UnifiedEngine *e, const char *json) {
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

  if (g_hash_table_remove(e->hls_generators, key)) {
    /* release encoder */
    stream_manager_release(e->stream_mgr, lens, tier);
    g_info("[HLS] Stopped HLS for %s", key);
    g_object_unref(parser);
    return g_strdup("{\"status\":\"stopped\"}");
  }

  g_object_unref(parser);
  return g_strdup(
      "{\"status\":\"error\",\"message\":\"HLS not running for branch\"}");
}

/* ── GetStatus (detailed, per-lens) ── */
char *engine_get_status(UnifiedEngine *e) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "total_clients");
  json_builder_add_int_value(b, e->total_clients);
  json_builder_set_member_name(b, "lenses");
  json_builder_begin_object(b);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, e->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lid = (const char *)key;
    LensInfo *info = (LensInfo *)value;

    json_builder_set_member_name(b, lid);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "device");
    json_builder_add_string_value(b, info->device);
    json_builder_set_member_name(b, "overlay_enabled");
    json_builder_add_boolean_value(b, info->overlay_enabled);

    /* Branches */
    json_builder_set_member_name(b, "branches");
    json_builder_begin_object(b);

    GHashTableIter cfg_iter;
    gpointer ck, cv;
    g_hash_table_iter_init(&cfg_iter, e->branch_configs);
    while (g_hash_table_iter_next(&cfg_iter, &ck, &cv)) {
      const char *cfg_key = (const char *)ck;
      if (!g_str_has_prefix(cfg_key, lid))
        continue;
      BranchConfig *cfg = (BranchConfig *)cv;
      const char *tier = strchr(cfg_key, '/');
      if (tier)
        tier++;

      char mount[64];
      snprintf(mount, sizeof(mount), "/%s", cfg_key);
      /* CapturePipeline *cap lookup removed (unused) */
      gboolean is_active =
          stream_manager_is_active(e->stream_mgr, lid, tier ? tier : cfg_key);

      json_builder_set_member_name(b, tier ? tier : cfg_key);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "mount");
      json_builder_add_string_value(b, mount);
      json_builder_set_member_name(b, "resolution");
      json_builder_add_string_value(b, cfg->resolution);
      json_builder_set_member_name(b, "fps");
      json_builder_add_int_value(b, cfg->fps);
      json_builder_set_member_name(b, "encoder");
      json_builder_add_string_value(b, cfg->codec);
      json_builder_set_member_name(b, "state");
      json_builder_add_string_value(b, is_active ? "active" : "idle");

      gboolean is_recording = FALSE;
      RecordingBranch *rec = g_hash_table_lookup(e->recordings, cfg_key);
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
char *engine_set_log_level(UnifiedEngine *e, const char *level) {
  (void)e;
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
char *engine_attach_shm(UnifiedEngine *e, const char *json) {
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

  CapturePipeline *cap = _get_capture(e, lens);
  if (!cap) {
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
  capture_pipeline_attach_ai_shm(cap, socket_path);

  g_info("[AI] Feed attached for %s. SHM socket: %s", lens, socket_path);

  char *resp = g_strdup_printf(
      "{\"status\":\"ok\",\"message\":\"AI SHM attached\",\"socket\":\"%s\"}",
      socket_path);

  g_object_unref(parser);
  return resp;
}

char *engine_detach_shm(UnifiedEngine *e, const char *json) {
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

  CapturePipeline *cap = _get_capture(e, lens);
  if (cap) {
    capture_pipeline_detach_ai_shm(cap);
    g_info("[AI] Feed detached for %s.", lens);
  }

  g_object_unref(parser);
  return g_strdup("{\"status\":\"ok\",\"message\":\"AI SHM detached\"}");
}

/* ── Toggle Overlay Interface ── */
char *engine_toggle_overlay(UnifiedEngine *e, const char *json) {
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

  JsonObject *request_obj = json_node_get_object(json_parser_get_root(json_parser));
  const char *lens_id =
      json_object_get_string_member_with_default(request_obj, "lens_id", "lens1");
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
  gboolean overlay_on = json_object_get_boolean_member(request_obj, "overlay_on");

  CapturePipeline *capture_pipe = _get_capture(e, lens_id);
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
    DynamicBranch *active_branch     = (DynamicBranch *)iter_val;

    /* If a specific tier_name was requested, skip others */
    if (tier_name && !g_str_has_suffix(pipeline_branch_name, tier_name)) {
      continue;
    }

    /* Guard: elements array may be NULL during async teardown */
    if (!active_branch->elements) {
      continue;
    }

    /* Find cairooverlay element and update its QData */
    for (int i = 0; i < dyn->num_elements; i++) {
      if (dyn->elements[i]) {
        GstElementFactory *f = gst_element_get_factory(dyn->elements[i]);
        if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)),
                           "cairooverlay") == 0) {
          g_object_set_data(G_OBJECT(dyn->elements[i]), "overlay_enabled",
                            GINT_TO_POINTER(overlay_on));
          overlays_toggled++;
          g_info("[AI] Overlay on tier_name '%s' set to %s", dyn_name,
                 overlay_on ? "ON" : "OFF");
        }
      }
    }
  }
  g_mutex_unlock(&capture_pipe->branch_mutex);

  if (overlays_toggled == 0 && tier_name) {
    g_warning("[AI] No cairooverlay found for %s/%s — tier_name may not exist or "
              "has no overlay",
              lens_id, tier_name);
  }
  g_info("[AI] Overlay toggle for %s/%s: %d overlays set to %s", lens_id,
         tier_name ? tier_name : "all", overlays_toggled, overlay_on ? "ON" : "OFF");

  _warn_overlay_toggle_limitations(e, lens_id, tier_name, overlay_on, capture_pipe);

  char *response;
  if (overlays_toggled == 0) {
    response = g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"Cairo overlay is removed from the "
        "pipeline. Please add it via ConfigureLens first.\"}");
  } else {
    response = g_strdup_printf("{\"status\":\"ok\",\"lens_id\":\"%s\",\"tier_name\":"
                           "\"%s\",\"overlay_enabled\":%s,\"count\":%d}",
                           lens_id, tier_name ? tier_name : "all",
                           overlay_on ? "true" : "false", overlays_toggled);
  }
  g_object_unref(json_parser);
  return response;
}

/* ── ToggleNTP ── */
char *engine_toggle_ntp(UnifiedEngine *e, const char *json) {
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

  JsonObject *request_obj = json_node_get_object(json_parser_get_root(json_parser));
  const char *lens_id =
      json_object_get_string_member_with_default(request_obj, "lens_id", "lens1");
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
    char cfg_key[128];
    snprintf(cfg_key, sizeof(cfg_key), "%s/%s", lens_id, tier_name);
    BranchConfig *branch_cfg = g_hash_table_lookup(e->branch_configs, cfg_key);
    if (branch_cfg) {
      branch_cfg->ntp_overlay = ntp_on;
    }
  }

  CapturePipeline *capture_pipe = _get_capture(e, lens_id);
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
    DynamicBranch *active_branch     = (DynamicBranch *)iter_val;

    /* If a specific tier_name was requested, skip others */
    if (tier_name && !g_str_has_suffix(pipeline_branch_name, tier_name)) {
      continue;
    }

    if (!active_branch->elements) {
      continue;
    }

    /* Find clockoverlay element and toggle its 'silent' property */
    for (int i = 0; i < dyn->num_elements; i++) {
      if (dyn->elements[i]) {
        GstElementFactory *f = gst_element_get_factory(dyn->elements[i]);
        if (f && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)),
                           "clockoverlay") == 0) {
          g_object_set(dyn->elements[i], "silent", !ntp_on, NULL);
          overlays_toggled++;
          g_info("[NTP] Overlay on tier_name '%s' set to %s", dyn_name,
                 ntp_on ? "ON" : "OFF");
        }
      }
    }
  }

  g_mutex_unlock(&capture_pipe->branch_mutex);
  _save_user_config(e);
  g_object_unref(json_parser);

  _warn_ntp_toggle_limitations(lens_id, tier_name, ntp_on, overlays_toggled);

  if (overlays_toggled == 0) {
    return g_strdup_printf(
        "{\"status\":\"error\",\"message\":\"NTP Clock overlay is removed from "
        "the pipeline. Please add it via ConfigureLens first.\"}");
  }

  return g_strdup_printf("{\"status\":\"success\",\"lens_id\":\"%s\",\"tier_name\":"
                         "\"%s\",\"ntp_on\":%s,\"overlays_toggled\":%d}",
                         lens_id, tier_name ? tier_name : "all",
                         ntp_on ? "true" : "false", overlays_toggled);
}

/* ── SaveCurrentConfig ── */
gboolean engine_save_config(UnifiedEngine *e) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, e->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lid = (const char *)key;
    json_builder_set_member_name(b, lid);
    json_builder_begin_object(b);

    /* RTSP configs */
    json_builder_set_member_name(b, "rtsp");
    json_builder_begin_object(b);
    GHashTableIter cfg_iter;
    gpointer ck, cv;
    g_hash_table_iter_init(&cfg_iter, e->branch_configs);
    while (g_hash_table_iter_next(&cfg_iter, &ck, &cv)) {
      if (!g_str_has_prefix((const char *)ck, lid))
        continue;
      BranchConfig *cfg = (BranchConfig *)cv;
      const char *tier = strchr((const char *)ck, '/');
      if (tier)
        tier++;
      json_builder_set_member_name(b, tier ? tier : "main");
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "resolution");
      json_builder_add_string_value(b, cfg->resolution);
      json_builder_set_member_name(b, "fps");
      json_builder_add_int_value(b, cfg->fps);
      json_builder_set_member_name(b, "encoder");
      json_builder_add_string_value(b, cfg->codec);
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
  if (g_file_get_contents(e->config_path, &existing, &elen, NULL)) {
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
  gboolean ok = json_generator_to_file(gen, e->config_path, NULL);

  g_object_unref(gen);
  g_object_unref(wb);
  g_object_unref(b);
  g_object_unref(parser);

  if (ok)
    g_info("Saved user config to %s", e->config_path);
  return ok;
}

/* ── Periodic DOT Dump (120s) ── */
static gboolean _dump_all_dots(gpointer data) {
  UnifiedEngine *e = (UnifiedEngine *)data;

  GDateTime *now = g_date_time_new_now_local();
  char *ts = g_date_time_format(now, "%H%M%S");
  g_date_time_unref(now);

  g_mutex_lock(&e->config_mutex);

  GHashTableIter iter;
  gpointer key, value;

  /* ── 1. Capture Pipelines ── */
  g_hash_table_iter_init(&iter, e->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lens_name = (const char *)key;
    LensInfo *info = (LensInfo *)value;
    CapturePipeline *cap = g_hash_table_lookup(e->captures, info->device);

    if (!cap)
      continue;

    guint n_branches = g_hash_table_size(cap->dynamic_branches);
    char dot_name[256];
    /* Use lens_name instead of device path to avoid slashes in the filename
     * [FIX-3] */
    snprintf(dot_name, sizeof(dot_name), "FULL_%s_br%u_%s", lens_name,
             n_branches, ts);
    capture_pipeline_dump_dot(cap, dot_name);

    g_info("  [%s] %u branches, cairo=%s, ai_coords=%d overlay=%s", lens_name,
           n_branches, cap->ai.cairo_capable ? "yes" : "no", cap->ai.num_coords,
           cap->ai.overlay_enabled ? "on" : "off");
  }

  /* ── 2. HLS Pipelines ── */
  if (e->hls_generators) {
    g_hash_table_iter_init(&iter, e->hls_generators);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      HLSGenerator *hls = (HLSGenerator *)value;
      if (!hls)
        continue;
      GstElement *pipe = hls_generator_get_pipeline(hls);
      if (pipe && GST_IS_ELEMENT(pipe)) {
        char name[128];
        snprintf(name, sizeof(name), "HLS_%s_%s", (const char *)key, ts);
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipe), GST_DEBUG_GRAPH_SHOW_ALL,
                                  name);
      }
    }
  }

  /* ── 3. RTSP Pipelines ── */
  if (e->rtsp_server) {
    GstRTSPMountPoints *mounts =
        gst_rtsp_server_get_mount_points(e->rtsp_server);
    if (mounts) {
      GHashTableIter cfg_iter;
      g_hash_table_iter_init(&cfg_iter, e->branch_configs);
      while (g_hash_table_iter_next(&cfg_iter, &key, &value)) {
        char mount_path[128];
        snprintf(mount_path, sizeof(mount_path), "/%s", (const char *)key);
        GstRTSPMediaFactory *factory =
            gst_rtsp_mount_points_match(mounts, mount_path, NULL);
        if (factory) {
          lazy_rtsp_factory_dump_dot(factory, ts);
          g_object_unref(factory);
        }
      }
      g_object_unref(mounts);
    }
  }

  /* ── 4. AI Engine Pipeline (via D-Bus) ── */
  /* [FIX-1] Declare gres and err in this scope to fix compile error */
  if (e->ai_proxy) {
    GError *err = NULL;
    GVariant *gres =
        g_dbus_proxy_call_sync(e->ai_proxy, "DumpDOT", g_variant_new("(s)", ts),
                               G_DBUS_CALL_FLAGS_NONE, 500, NULL, &err);

    if (gres) {
      g_variant_unref(gres);
    }
    if (err) {
      g_debug("AI DumpDOT skipped: %s", err->message);
      g_error_free(err);
    }
  }

  g_mutex_unlock(&e->config_mutex);
  g_free(ts);
  return G_SOURCE_CONTINUE;
}

/* ══════════════════════════════════════════════════════════════
 *  System Stats (port of _log_system_stats — 15s periodic)
 * ══════════════════════════════════════════════════════════════ */

static gboolean _log_system_stats(gpointer data) {
  UnifiedEngine *e = (UnifiedEngine *)data;

  /* ── 1. Reap Idle HLS Generators ── */
  GHashTableIter hls_iter;
  gpointer hk, hv;
  GList *hls_to_remove = NULL;

  g_hash_table_iter_init(&hls_iter, e->hls_generators);
  while (g_hash_table_iter_next(&hls_iter, &hk, &hv)) {
    HLSGenerator *hls = (HLSGenerator *)hv;
    if (hls_generator_get_idle_time_sec(hls) > 30) {
      g_info("[HLS Reaper] Reaping idle HLS stream for %s (no viewers for 30s)",
             (const char *)hk);
      hls_to_remove = g_list_append(hls_to_remove, g_strdup((const char *)hk));
    }
  }

  for (GList *l = hls_to_remove; l; l = l->next) {
    const char *key = (const char *)l->data;
    g_hash_table_remove(e->hls_generators, key);
    /* Release StreamManager encoder */
    char **parts = g_strsplit(key, "/", 2);
    if (parts[0] && parts[1]) {
      stream_manager_release(e->stream_mgr, parts[0], parts[1]);
    }
    g_strfreev(parts);
    g_free(l->data);
  }
  g_list_free(hls_to_remove);

  /* ── 2. Standard Logging ── */
  g_info("==== UNIFIED ENGINE STATUS ====");
  g_info("Total RTSP Clients: %d", e->total_clients);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, e->lenses);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *lid = (const char *)key;
    CapturePipeline *cap = _get_capture(e, lid);
    int dyn_count = cap ? (int)g_hash_table_size(cap->dynamic_branches) : 0;
    g_info(" Lens [%s] → Dynamic branches: %d", lid, dyn_count);
  }
  _audit_overlay_pipeline_elements(e);
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
  UnifiedEngine *e = (UnifiedEngine *)data;
  const gchar *sid = gst_rtsp_session_get_sessionid(session);

  gchar *mount = g_hash_table_lookup(e->session_mounts, sid);
  if (mount) {
    gpointer count_ptr = g_hash_table_lookup(e->active_branches, mount);
    int count = GPOINTER_TO_INT(count_ptr);
    g_hash_table_insert(e->active_branches, g_strdup(mount),
                        GINT_TO_POINTER(count > 0 ? count - 1 : 0));
    e->total_clients = e->total_clients > 0 ? e->total_clients - 1 : 0;
    g_hash_table_remove(e->session_mounts, sid);
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
  UnifiedEngine *e = (UnifiedEngine *)user_data;
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
    CapturePipeline *cap = _get_capture(e, lens);
    if (!cap) {
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

    g_mutex_lock(&cap->ai.mutex);
    cap->ai.ml_w = ml_w;
    cap->ai.ml_h = ml_h;

    /* [CPU-OPT-5] Reuse the existing allocation when the incoming detection
     * count matches.  At 25 fps with a stationary subject the count is
     * constant frame-to-frame, so this eliminates ~25 g_free/g_new0 pairs
     * per second per lens.  Only reallocate when the count actually changes. */
    if ((guint)cap->ai.num_coords != length) {
      g_free(cap->ai.coords);
      cap->ai.coords = length > 0 ? g_new0(AICoord, length) : NULL;
      cap->ai.num_coords = (int)length;
    }

    if (length > 0 && cap->ai.coords) {
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
            cap->ai.coords[i].left = json_array_get_double_element(bbox, 0);
            cap->ai.coords[i].top = json_array_get_double_element(bbox, 1);
            cap->ai.coords[i].right = json_array_get_double_element(bbox, 2);
            cap->ai.coords[i].bottom = json_array_get_double_element(bbox, 3);
          }
        }
        cap->ai.coords[i].conf =
            json_object_get_double_member_with_default(c_obj, "conf", 0.0);
        const char *label = json_object_get_string_member_with_default(
            c_obj, "class_name", "unknown");
        g_strlcpy(cap->ai.coords[i].label, label,
                  sizeof(cap->ai.coords[i].label));
      }
    }
    g_mutex_unlock(&cap->ai.mutex);

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
     *    carry a "snapshot" path (i.e. the AI engine did not save one, e.g.
     *    ai_snapshots_enabled=false), the C engine saves its own JPEG from the
     *    live appsink.  A per-lens cooldown prevents disk floods at full fps.
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
    gpointer rk, rv;
    g_hash_table_iter_init(&iter, e->recordings);
    while (g_hash_table_iter_next(&iter, &rk, &rv)) {
      RecordingBranch *rec = (RecordingBranch *)rv;
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
        char *ts_str = g_date_time_format(dt, "%Y%m%d_%H%M%S");
        g_date_time_unref(dt);

        char snap_path[512];
        snprintf(snap_path, sizeof(snap_path),
                 DEFAULT_DATA_DIR "/snapshots/%s_ai_%s.jpg", lens, ts_str);
        g_free(ts_str);

        /* engine_take_snapshot creates the directory, encodes via GStreamer,
         * and returns a JSON status string. It is safe to call from the ZMQ
         * callback thread because it builds a self-contained mini-pipeline
         * with its own GMainLoop bus wait. */
        char *snap_result = engine_take_snapshot(e, lens, snap_path);
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
