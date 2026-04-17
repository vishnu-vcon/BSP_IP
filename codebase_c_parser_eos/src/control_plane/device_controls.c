/*
 * device_controls.c
 *
 * Device control logic for the SmartCamera server.
 *
 * Functions:
 *   device_reboot()   — reboot the device
 *   device_status()   — uptime, storage, RAM, network
 *   network_reset()   — restart networking service
 *   config_reset()    — restore config to defaults
 *   factory_reset()   — wipe all config and user data
 */

#include "device_controls.h"
#include "auth/activity_logger.h"  /* FIX: activity_log_event() used below but header was missing — caused implicit-declaration error */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define CAMERA_STORAGE_PATH "/media/recordings"

/* ─── helpers ──────────────────────────────────────────────────────────── */

static double bytes_to_gb(unsigned long long b)
{
    return (double)b / (1024.0 * 1024.0 * 1024.0);
}

static double kb_to_gb(unsigned long long kb)
{
    return (double)kb / (1024.0 * 1024.0);
}

/* Ping 8.8.8.8 — returns "ONLINE" or "OFFLINE" */
static const char *get_device_online_status(void)
{
    int rc = system("ping -c 1 -W 2 8.8.8.8 > /dev/null 2>&1");
    return (rc == 0) ? "ONLINE" : "OFFLINE";
}

/* Fill buf with JSON for one disk path */
static void storage_json(const char *path, char *buf, size_t buf_size)
{
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) {
        snprintf(buf, buf_size,
                 "{\"path\":\"%s\",\"status\":\"NOT MOUNTED\","
                 "\"total\":\"0 GB\",\"used\":\"0 GB\","
                 "\"free\":\"0 GB\",\"percent\":\"0%% used\"}",
                 path);
        return;
    }
    unsigned long long total    = (unsigned long long)sv.f_blocks * sv.f_frsize;
    unsigned long long free_b   = (unsigned long long)sv.f_bfree  * sv.f_frsize;
    unsigned long long used_b   = total - free_b;
    int                pct      = (int)(total ? (used_b * 100 / total) : 0);
    snprintf(buf, buf_size,
             "{\"total\":\"%.2f GB\",\"used\":\"%.2f GB\","
             "\"free\":\"%.2f GB\",\"percent\":\"%d%% used\"}",
             bytes_to_gb(total), bytes_to_gb(used_b),
             bytes_to_gb(free_b), pct);
}

/* Fill buf with JSON for RAM from /proc/meminfo */
static void ram_json(char *buf, size_t buf_size)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        snprintf(buf, buf_size,
                 "{\"total\":\"unavailable\",\"used\":\"unavailable\","
                 "\"free\":\"unavailable\",\"percent\":\"unavailable\"}");
        return;
    }
    unsigned long long mem_total = 0, mem_avail = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long val = 0;
        if (sscanf(line, "MemTotal: %llu", &val) == 1)      mem_total = val;
        if (sscanf(line, "MemAvailable: %llu", &val) == 1)  mem_avail = val;
    }
    fclose(f);
    unsigned long long used = mem_total - mem_avail;
    int pct = (int)(mem_total ? (used * 100 / mem_total) : 0);
    snprintf(buf, buf_size,
             "{\"total\":\"%.2f GB\",\"used\":\"%.2f GB\","
             "\"free\":\"%.2f GB\",\"percent\":\"%d%% used\"}",
             kb_to_gb(mem_total), kb_to_gb(used),
             kb_to_gb(mem_avail), pct);
}

/* Append JSON array of network interfaces to buf (max buf_size bytes).
 * Returns number of chars written. */
static int network_json(char *buf, size_t buf_size)
{
    int written = snprintf(buf, buf_size, "[");
    const char *net_path = "/sys/class/net";
    DIR *d = opendir(net_path);
    if (!d) {
        written += snprintf(buf + written, buf_size - written,
                            "{\"interface\":\"unknown\","
                            "\"status\":\"OFFLINE\","
                            "\"ip\":\"unavailable\"}]");
        return written;
    }

    int first = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *iface = ent->d_name;
        if (strcmp(iface, ".") == 0 || strcmp(iface, "..") == 0
            || strcmp(iface, "lo") == 0)
            continue;

        /* Read operstate */
        char state_path[512];
        snprintf(state_path, sizeof(state_path),
                 "%s/%s/operstate", net_path, iface);
        char state[32] = "UNKNOWN";
        FILE *sf = fopen(state_path, "r");
        if (sf) { fscanf(sf, "%31s", state); fclose(sf); }
        for (char *p = state; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32; /* toupper */
        const char *conn = (strcmp(state, "UP") == 0) ? "ONLINE" : "OFFLINE";

        /* Get IP with `ip -4 addr show <iface>` */
        char ip_addr[64] = "no IP assigned";
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "ip -4 addr show %s 2>/dev/null", iface);
        FILE *pp = popen(cmd, "r");
        if (pp) {
            char ln[256];
            while (fgets(ln, sizeof(ln), pp)) {
                char *p2 = ln;
                while (*p2 == ' ' || *p2 == '\t') p2++;
                if (strncmp(p2, "inet ", 5) == 0) {
                    sscanf(p2 + 5, "%63s", ip_addr);
                    break;
                }
            }
            pclose(pp);
        }

        if (!first) written += snprintf(buf + written, buf_size - written, ",");
        first = 0;
        written += snprintf(buf + written, buf_size - written,
                            "{\"interface\":\"%s\","
                            "\"status\":\"%s\","
                            "\"ip\":\"%s\"}",
                            iface, conn, ip_addr);
    }
    closedir(d);
    written += snprintf(buf + written, buf_size - written, "]");
    return written;
}

/* ═══════════════════════════════════════════════════════════
 *  1. Reboot
 * ═══════════════════════════════════════════════════════════ */

int device_reboot(const Session *session, const char *ip,
                  char *result_buf, size_t result_buf_size)
{
    printf("  [DeviceReboot] requested  by=%s  role=%s\n",
           session->user, session->role);
    activity_log_event(session->user, "device_reboot",
              "Device reboot requested.", ip);

    /* Non-blocking reboot */
    if (fork() == 0) {
        execl("/sbin/reboot", "reboot", NULL);
        execl("/usr/sbin/reboot", "reboot", NULL);
        _exit(1);
    }

    snprintf(result_buf, result_buf_size,
             "{\"message\":\"device reboot initiated\"}");
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  2. Device Status
 * ═══════════════════════════════════════════════════════════ */

int device_status(const Session *session, const char *ip,
                  char *result_buf, size_t result_buf_size)
{
    /* Uptime */
    double uptime_sec = 0.0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { fscanf(f, "%lf", &uptime_sec); fclose(f); }
    int hours   = (int)uptime_sec / 3600;
    int minutes = ((int)uptime_sec % 3600) / 60;

    const char *online_status = get_device_online_status();

    char sd_json[256], cam_json[512], ram_buf[256], net_buf[2048];
    storage_json("/",                sd_json,  sizeof(sd_json));

    /* Camera storage: add path key */
    struct stat st;
    if (stat(CAMERA_STORAGE_PATH, &st) == 0) {
        char tmp[256];
        storage_json(CAMERA_STORAGE_PATH, tmp, sizeof(tmp));
        /* Insert path into JSON */
        snprintf(cam_json, sizeof(cam_json),
                 "{\"path\":\"%s\",%s", CAMERA_STORAGE_PATH, tmp + 1);
    } else {
        snprintf(cam_json, sizeof(cam_json),
                 "{\"path\":\"%s\",\"status\":\"NOT MOUNTED\","
                 "\"total\":\"0 GB\",\"used\":\"0 GB\","
                 "\"free\":\"0 GB\",\"percent\":\"0%% used\"}",
                 CAMERA_STORAGE_PATH);
    }

    ram_json(ram_buf, sizeof(ram_buf));
    network_json(net_buf, sizeof(net_buf));

    activity_log_event(session->user, "device_status",
              "Device status read.", ip);

    snprintf(result_buf, result_buf_size,
             "{"
             "\"device_status\":\"%s\","
             "\"uptime\":\"%dh %dm\","
             "\"storage\":{"
             "\"sd_emmc\":%s,"
             "\"camera_recordings\":%s,"
             "\"ram\":%s"
             "},"
             "\"network\":%s,"
             "\"role\":\"%s\""
             "}",
             online_status,
             hours, minutes,
             sd_json, cam_json, ram_buf,
             net_buf,
             session->role);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  3. Network Reset
 * ═══════════════════════════════════════════════════════════ */

int network_reset(const Session *session, const char *ip,
                  char *result_buf, size_t result_buf_size)
{
    printf("  [NetworkReset] requested  by=%s  role=%s\n",
           session->user, session->role);
    activity_log_event(session->user, "network_reset",
              "Network reset requested.", ip);

    system("systemctl restart connman");

    snprintf(result_buf, result_buf_size,
             "{\"message\":\"network reset initiated\"}");
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  4. Config Reset
 * ═══════════════════════════════════════════════════════════ */

int config_reset(const Session *session, const char *ip,
                 char *result_buf, size_t result_buf_size)
{
    printf("  [ConfigReset] requested  by=%s  role=%s\n",
           session->user, session->role);
    activity_log_event(session->user, "config_reset",
              "Config reset to defaults.", ip);

    /* Add config restore logic here, e.g.:
     * copy("config/defaults.json", "config/current.json"); */

    snprintf(result_buf, result_buf_size,
             "{\"message\":\"config reset to defaults\"}");
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  5. Factory Reset
 * ═══════════════════════════════════════════════════════════ */

int factory_reset(const Session *session, const char *ip,
                  int confirm,
                  char *result_buf, size_t result_buf_size)
{
    if (!confirm) return -2; /* caller returns HTTP 400 */

    printf("  [FactoryReset] requested  by=%s  role=%s\n",
           session->user, session->role);
    activity_log_event(session->user, "factory_reset",
              "Factory reset initiated.", ip);

    /* Add wipe/restore logic here */

    snprintf(result_buf, result_buf_size,
             "{\"message\":\"factory reset initiated\"}");
    return 0;
}
