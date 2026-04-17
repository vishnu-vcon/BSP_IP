/*
 * auth/activity_logger.c
 *
 * User Activity Logger.
 * Appends JSON Lines to /data/device_logs/user_activity.log.
 * Falls back to ./user_activity.log on permission error.
 *
 * ALSO mirrors every entry to stdout (terminal) in a human-readable
 * colour-coded format:
 *
 *   [2026-04-01 12:34:56 UTC] [LOGIN      ] user=admin  details=Successful login via TOTP | ip=192.168.1.10
 *   [2026-04-01 12:34:57 UTC] [LOGIN_FAIL ] user=bob    details=Wrong password | ip=10.0.0.5
 *
 * Example JSON line written to file:
 *   {"timestamp":"2026-04-01T12:34:56Z","user_id":"admin",
 *    "action":"login","details":"Successful login via TOTP | ip=192.168.1.10"}
 *
 * FIXES applied vs original:
 *   1. LOG_PATH_PRIMARY changed to /data/device_logs/user_activity.log
 *      (matches LOG_DIR used by log_init() in server.c — no more mismatch).
 *   2. Terminal mirror added inside log_user_activity() so every event is
 *      visible immediately in the console with timestamp + colour coding.
 *   3. log_event() guards against NULL details before calling strstr().
 */

#include "activity_logger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

/* ── Path config ─────────────────────────────────────────────────────────── */
/*  PRIMARY now matches LOG_DIR ("/data/device_logs") used in server.c so the
 *  startup message "Activity log → /data/device_logs/user_activity.log" is
 *  actually correct.                                                          */
#define LOG_PATH_PRIMARY  "/data/device_logs/user_activity.log"
#define LOG_PATH_FALLBACK "./user_activity.log"

#define DETAILS_MAX  512
#define LINE_MAX_LEN 1024

static char log_file[256] = LOG_PATH_PRIMARY;
static int  log_initialized = 0;

/* ── ANSI colour codes for terminal output ──────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_CYAN    "\033[36m"   /* timestamp */
#define COL_GREEN   "\033[32m"   /* successful actions  */
#define COL_YELLOW  "\033[33m"   /* warnings / attempts */
#define COL_RED     "\033[31m"   /* failures / denials  */
#define COL_MAGENTA "\033[35m"   /* system events       */
#define COL_BOLD    "\033[1m"

/* Pick a colour for the action label based on keywords in the action string */
static const char *action_colour(const char *action)
{
    if (!action) return COL_RESET;

    /* success */
    if (strstr(action, "SUCCESS") || strstr(action, "login") == action ||
        strstr(action, "SENT")    || strstr(action, "created") ||
        strstr(action, "reset")   || strstr(action, "SERVER_START"))
        return COL_GREEN;

    /* hard failures / denials */
    if (strstr(action, "FAIL")   || strstr(action, "DENIED") ||
        strstr(action, "WRONG")  || strstr(action, "INVALID") ||
        strstr(action, "REBOOT") || strstr(action, "FACTORY"))
        return COL_RED;

    /* mid-level warnings */
    if (strstr(action, "OTP")    || strstr(action, "TOTP") ||
        strstr(action, "FORGOT") || strstr(action, "RESET") ||
        strstr(action, "REMOVED"))
        return COL_YELLOW;

    /* system / misc */
    if (strstr(action, "SERVER") || strstr(action, "NETWORK") ||
        strstr(action, "CONFIG"))
        return COL_MAGENTA;

    return COL_RESET;
}

/* ── JSON escaping ──────────────────────────────────────────────────────── */
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_size; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= dst_size) break;
            dst[j++] = '\\';
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

/* ── Ensure log file exists and is writable ─────────────────────────────── */
static void ensure_log_file(void)
{
    if (log_initialized) return;
    log_initialized = 1;

    FILE *f = fopen(log_file, "a");
    if (!f) {
        if (errno == EACCES || errno == ENOENT) {
            fprintf(stderr,
                    "  [Logger] WARNING: cannot write to %s — using %s\n",
                    LOG_PATH_PRIMARY, LOG_PATH_FALLBACK);
            snprintf(log_file, sizeof(log_file), "%s", LOG_PATH_FALLBACK);
        }
        return;
    }
    fclose(f);
    chmod(log_file, 0644);
}

/* ── Core write + terminal mirror ───────────────────────────────────────── */
void log_user_activity(const char *user_id,
                       const char *action,
                       const char *details)
{
    ensure_log_file();

    /* ── Build ISO-8601 UTC timestamp ── */
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);

    char ts_iso[32];   /* "2026-04-01T12:34:56Z"   — for JSON file */
    char ts_hr[32];    /* "2026-04-01 12:34:56 UTC" — for terminal  */
    strftime(ts_iso, sizeof(ts_iso), "%Y-%m-%dT%H:%M:%SZ",    &tm_utc);
    strftime(ts_hr,  sizeof(ts_hr),  "%Y-%m-%d %H:%M:%S UTC", &tm_utc);

    /* ── Escape for JSON ── */
    char uid_esc[128], act_esc[128], det_esc[DETAILS_MAX];
    json_escape(user_id  ? user_id  : "", uid_esc, sizeof(uid_esc));
    json_escape(action   ? action   : "", act_esc, sizeof(act_esc));
    json_escape(details  ? details  : "", det_esc, sizeof(det_esc));

    /* ── Build JSON line ── */
    char line[LINE_MAX_LEN];
    snprintf(line, sizeof(line),
             "{\"timestamp\":\"%s\",\"user_id\":\"%s\","
             "\"action\":\"%s\",\"details\":\"%s\"}\n",
             ts_iso, uid_esc, act_esc, det_esc);

    /* ── Write to log file ── */
    FILE *f = fopen(log_file, "a");
    if (!f) {
        fprintf(stderr, "  [Logger] FAILED to open log: %s\n", log_file);
        fprintf(stderr, "  [Logger] Entry was: %s", line);
    } else {
        fputs(line, f);
        fclose(f);
    }

    /* ── Mirror to terminal (stdout) ─────────────────────────────────────
     *
     *  Format:
     *  [2026-04-01 12:34:56 UTC] [ACTION_NAME      ] user=admin  details=...
     *
     *  Action label is padded to 20 chars for alignment.
     *  Colour-coded by action type.
     * ────────────────────────────────────────────────────────────────── */
    const char *col = action_colour(action ? action : "");
    printf("%s[%s]%s %s%-20s%s user=%-16s details=%s\n",
           COL_CYAN, ts_hr, COL_RESET,
           col, action ? action : "(null)", COL_RESET,
           user_id ? user_id : "(null)",
           details ? details : "");

    /* Flush immediately so output appears even if stdout is line-buffered */
    fflush(stdout);
}

/* ── Wrapper: appends IP to details if not already present ─────────────── */
void activity_log_event(const char *user_id,
               const char *action,
               const char *details,
               const char *ip)
{
    /* Guard: details may be NULL — original strstr(details, ip) would crash */
    if (ip && ip[0] && (!details || strstr(details, ip) == NULL)) {
        char combined[DETAILS_MAX];
        if (details && details[0])
            snprintf(combined, sizeof(combined), "%s | ip=%s", details, ip);
        else
            snprintf(combined, sizeof(combined), "ip=%s", ip);
        log_user_activity(user_id, action, combined);
    } else {
        log_user_activity(user_id, action, details ? details : "");
    }
}
