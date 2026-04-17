/* ============================================================================
 * sys_ntp.c  –  Shared NTP time-sync service
 *
 * FIXES APPLIED:
 *   [FIX-1] _format_ist() used gmtime() which returns a pointer to a
 *           process-wide static struct tm. Both the NTP sync thread and the
 *           ticker thread called this concurrently — on the quad-core Cortex-A53
 *           (imx8mp) this is a real data race corrupting timestamp strings in
 *           video overlays. Replaced with gmtime_r() (POSIX re-entrant variant).
 *
 *   [FIX-2] g_stop_flag and g_running were declared as volatile int. On ARM SMP,
 *           volatile only prevents compiler register caching — it does NOT emit
 *           a DMB (data memory barrier) instruction, so a write on one core may
 *           not be visible to another for an undefined number of cycles. Replaced
 *           with GLib atomic operations (g_atomic_int_set / g_atomic_int_get)
 *           which include the necessary memory fence on ARM.
 * ============================================================================ */

#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include "subsystems/sys_ntp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <glib.h>

/* ── NTP protocol constants ── */
#define NTP_PORT          123
#define NTP_PACKET_SIZE   48
#define NTP_UNIX_OFFSET   2208988800UL

typedef struct {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    uint8_t  precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;  uint32_t ref_ts_frac;
    uint32_t orig_ts_sec; uint32_t orig_ts_frac;
    uint32_t rx_ts_sec;   uint32_t rx_ts_frac;
    uint32_t tx_ts_sec;   uint32_t tx_ts_frac;
} ntp_packet_t;

/* ── Internal shared state ── */
static char            g_ist_timestamp[NTP_TIMESTAMP_LEN] = "Time not synced";
/* [FIX-2] Use gint with GLib atomics instead of volatile int */
static gint            g_ntp_synced = 0;
static gint            g_running    = 0;
static gint            g_stop_flag  = 0;

static pthread_mutex_t g_ts_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_ntp_tid    = 0;
static pthread_t       g_tick_tid   = 0;

/* ── Internal helpers ── */

static time_t _query_ntp_server(const char *server) {
    int sockfd;
    ntp_packet_t      packet;
    struct sockaddr_in serv_addr;
    struct addrinfo    hints, *res;
    struct timeval     timeout = {5, 0};
    socklen_t          addr_len;
    uint32_t           tx_sec;

    memset(&packet, 0, sizeof(packet));
    packet.li_vn_mode = (0 << 6) | (3 << 3) | 3;

    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) return 0;

    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(server, NULL, &hints, &res) != 0) {
        g_warning("[SYS_NTP] DNS resolution failed for %s", server);
        close(sockfd);
        return 0;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(NTP_PORT);
    serv_addr.sin_addr   = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    if (sendto(sockfd, &packet, sizeof(packet), 0,
               (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd); return 0;
    }

    addr_len = sizeof(serv_addr);
    if (recvfrom(sockfd, &packet, sizeof(packet), 0,
                 (struct sockaddr *)&serv_addr, &addr_len) < 0) {
        close(sockfd); return 0;
    }
    close(sockfd);

    tx_sec = ntohl(packet.tx_ts_sec);
    if (tx_sec < NTP_UNIX_OFFSET) return 0;
    return (time_t)(tx_sec - NTP_UNIX_OFFSET);
}

/* [FIX-1] Use gmtime_r — stack-local struct tm, safe for concurrent calls */
static void _format_ist(time_t utc, char *buf, size_t len) {
    time_t    ist    = utc + IST_OFFSET_SEC;
    struct tm tm_buf;                    /* stack-local: no shared state */
    gmtime_r(&ist, &tm_buf);            /* POSIX re-entrant variant */
    strftime(buf, len, "%d-%m-%Y %H:%M:%S IST", &tm_buf);
}

static void _update_timestamp_locked(const char *buf) {
    pthread_mutex_lock(&g_ts_mutex);
    strncpy(g_ist_timestamp, buf, NTP_TIMESTAMP_LEN - 1);
    g_ist_timestamp[NTP_TIMESTAMP_LEN - 1] = '\0';
    pthread_mutex_unlock(&g_ts_mutex);
}

/* ── Background threads ── */

static void *_ntp_sync_thread(void *arg) {
    (void)arg;
    g_info("[SYS_NTP] Sync daemon started → server: %s", NTP_SERVER);

    /* [FIX-2] Use g_atomic_int_get for cross-core visibility */
    while (!g_atomic_int_get(&g_stop_flag)) {
        time_t utc = _query_ntp_server(NTP_SERVER);

        if (utc > 0) {
            struct timespec ts = {utc, 0};
            if (clock_settime(CLOCK_REALTIME, &ts) == 0)
                g_message("[SYS_NTP] OS clock updated.");
            else
                g_warning("[SYS_NTP] clock_settime failed (needs CAP_SYS_TIME).");

            char buf[NTP_TIMESTAMP_LEN];
            _format_ist(utc, buf, sizeof(buf));
            _update_timestamp_locked(buf);

            pthread_mutex_lock(&g_ts_mutex);
            g_atomic_int_set(&g_ntp_synced, 1);
            pthread_mutex_unlock(&g_ts_mutex);

            for (int i = 0; i < NTP_SYNC_INTERVAL && !g_atomic_int_get(&g_stop_flag); i++)
                sleep(1);
        } else {
            g_warning("[SYS_NTP] Network failure. Retrying in %ds", NTP_RETRY_INTERVAL);
            for (int i = 0; i < NTP_RETRY_INTERVAL && !g_atomic_int_get(&g_stop_flag); i++)
                sleep(1);
        }
    }

    g_message("[SYS_NTP] Sync thread exiting.");
    return NULL;
}

static void *_ntp_ticker_thread(void *arg) {
    (void)arg;
    while (!g_atomic_int_get(&g_stop_flag)) {
        sleep(1);
        if (g_atomic_int_get(&g_ntp_synced)) {
            time_t now = time(NULL);
            char   buf[NTP_TIMESTAMP_LEN];
            _format_ist(now, buf, sizeof(buf));   /* [FIX-1] thread-safe */
            _update_timestamp_locked(buf);
        }
    }
    return NULL;
}

/* ── Public API ── */

ntp_err_t sys_ntp_start(void) {
    /* [FIX-2] g_atomic_int_get for proper visibility */
    if (g_atomic_int_get(&g_running)) return NTP_ERR_ALREADY;

    g_atomic_int_set(&g_stop_flag, 0);
    g_atomic_int_set(&g_running,   1);

    if (pthread_create(&g_ntp_tid, NULL, _ntp_sync_thread, NULL) != 0) {
        g_atomic_int_set(&g_running, 0);
        return NTP_ERR_THREAD;
    }
    pthread_detach(g_ntp_tid);

    if (pthread_create(&g_tick_tid, NULL, _ntp_ticker_thread, NULL) != 0) {
        g_atomic_int_set(&g_stop_flag, 1);
        g_atomic_int_set(&g_running,   0);
        return NTP_ERR_THREAD;
    }
    pthread_detach(g_tick_tid);

    return NTP_OK;
}

void sys_ntp_stop(void) {
    if (!g_atomic_int_get(&g_running)) return;
    g_atomic_int_set(&g_stop_flag, 1);
    g_atomic_int_set(&g_running,   0);
}

void sys_ntp_get_timestamp(char *buf, size_t len) {
    pthread_mutex_lock(&g_ts_mutex);
    strncpy(buf, g_ist_timestamp, len - 1);
    buf[len - 1] = '\0';
    pthread_mutex_unlock(&g_ts_mutex);
}

time_t sys_ntp_get_unix_time(void) {
    return g_atomic_int_get(&g_ntp_synced) ? time(NULL) : 0;
}

int sys_ntp_is_synced(void) {
    return g_atomic_int_get(&g_ntp_synced);
}

void sys_ntp_get_status(char *ts_buf, size_t len, int *synced) {
    pthread_mutex_lock(&g_ts_mutex);
    strncpy(ts_buf, g_ist_timestamp, len - 1);
    ts_buf[len - 1] = '\0';
    *synced = g_atomic_int_get(&g_ntp_synced);
    pthread_mutex_unlock(&g_ts_mutex);
}
