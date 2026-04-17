/* ============================================================================
 * sys_ntp.h  –  Shared NTP time-sync service
 *
 * Provides a thread-safe, background-synchronised IST clock that seamlessly
 * integrates into the SmartIP Edge Engine layered architecture.
 *
 * It uses pure C sockets to query NTP and modifies the Linux System Realtime
 * clock directly, allowing natively compiled GStreamer elements to automatically
 * inherit correct epoch logic without manual string parsing overlays.
 * ============================================================================ */

#ifndef SYS_NTP_H
#define SYS_NTP_H

#include <stddef.h>   /* size_t  */
#include <time.h>     /* time_t  */

/* ── Compile-time configuration ────────── */
#ifndef NTP_SERVER
#  define NTP_SERVER         "time3.google.com"   
#endif

#ifndef NTP_SYNC_INTERVAL
#  define NTP_SYNC_INTERVAL  3600                 /* full re-sync period (s)  */
#endif

#ifndef NTP_RETRY_INTERVAL
#  define NTP_RETRY_INTERVAL 60                   /* retry delay on failure   */
#endif

#ifndef IST_OFFSET_SEC
#  define IST_OFFSET_SEC     (5*3600 + 30*60)     /* IST = UTC + 05:30        */
#endif

#define NTP_TIMESTAMP_LEN    64   

/* ── Return codes ─────────────────────────────────────────────────────────── */
typedef enum {
    NTP_OK              =  0,   
    NTP_ERR_THREAD      = -1,   
    NTP_ERR_ALREADY     = -2,   
} ntp_err_t;

/* ═══════════════════════════════════════════════════════════════════════════
   Lifecycle
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * sys_ntp_start()
 * Spawns the NTP sync thread securely in the background.
 */
ntp_err_t sys_ntp_start(void);

/**
 * sys_ntp_stop()
 * Signals the background daemon to exit gracefully.
 */
void sys_ntp_stop(void);

/* ═══════════════════════════════════════════════════════════════════════════
   Time getters (Strictly Thread-safe)
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * sys_ntp_get_timestamp()
 * Resolves the string formatting of the time if needed by REST.
 */
void sys_ntp_get_timestamp(char *buf, size_t len);

/**
 * sys_ntp_get_unix_time()
 * Returns raw timestamp for any subsystem requirement.
 */
time_t sys_ntp_get_unix_time(void);

int sys_ntp_is_synced(void);

void sys_ntp_get_status(char *ts_buf, size_t len, int *synced);

#endif /* SYS_NTP_H */
