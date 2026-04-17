/*
 * smtp_sender.c
 * -------------
 * SMTP email service for the AI Camera pipeline.
 * Called by the Alert Manager via send_snapshots().
 *
 * Depends on libcurl (link with -lcurl).
 */

#include "smtp_sender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <libgen.h>
#include <curl/curl.h>

/* ── Logging helpers ───────────────────────────────────────────────────── */

#define LOG_INFO(fmt, ...)  do { \
    time_t _t = time(NULL); struct tm _tm; localtime_r(&_t, &_tm); \
    char _ts[20]; strftime(_ts, sizeof(_ts), "%Y-%m-%d %H:%M:%S", &_tm); \
    fprintf(stderr, "%s [smtp_sender] INFO  " fmt "\n", _ts, ##__VA_ARGS__); \
} while (0)

#define LOG_WARN(fmt, ...)  do { \
    time_t _t = time(NULL); struct tm _tm; localtime_r(&_t, &_tm); \
    char _ts[20]; strftime(_ts, sizeof(_ts), "%Y-%m-%d %H:%M:%S", &_tm); \
    fprintf(stderr, "%s [smtp_sender] WARN  " fmt "\n", _ts, ##__VA_ARGS__); \
} while (0)

#define LOG_ERROR(fmt, ...) do { \
    time_t _t = time(NULL); struct tm _tm; localtime_r(&_t, &_tm); \
    char _ts[20]; strftime(_ts, sizeof(_ts), "%Y-%m-%d %H:%M:%S", &_tm); \
    fprintf(stderr, "%s [smtp_sender] ERROR " fmt "\n", _ts, ##__VA_ARGS__); \
} while (0)

/* ── String helpers ────────────────────────────────────────────────────── */

static void str_trim(char *s)
{
    /* trim leading */
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    /* trim trailing */
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static int str_to_bool(const char *s)
{
    if (!s || !*s) return 0;
    return (strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
            strcasecmp(s, "1") == 0);
}



static const char *basename_const(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ── Config loader ─────────────────────────────────────────────────────── */

int load_config(const char *config_path, SmtpConfig *cfg)
{
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOG_ERROR("Config file not found: %s", config_path);
        LOG_ERROR("Create it from the template in the README, then re-run.");
        return -1;
    }

    /* Defaults */
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->server.host, "smtp.gmail.com", MAX_VALUE_LEN - 1);
    cfg->server.port = 587;
    cfg->server.use_ssl = false;
    strncpy(cfg->subject, "AI Camera - Snapshot Alert", MAX_SUBJECT_LEN - 1);
    strncpy(cfg->snapshots_dir, "/var/lib/camera/snapshots", MAX_PATH_LEN - 1);
    cfg->max_attach_bytes = 25 * 1024 * 1024;
    cfg->max_per_email    = 10;
    cfg->recipient_count  = 0;

    /* Alert defaults */
    cfg->intrusion_enabled = true;
    strncpy(cfg->intrusion_priority, "medium", MAX_VALUE_LEN - 1);
    cfg->intrusion_cooldown = 60;
    cfg->loitering_enabled = true;
    strncpy(cfg->loitering_priority, "medium", MAX_VALUE_LEN - 1);
    cfg->loitering_cooldown = 60;
    cfg->motion_enabled = true;
    strncpy(cfg->motion_priority, "medium", MAX_VALUE_LEN - 1);
    cfg->motion_cooldown = 60;

    char line[1024];
    char section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        /* strip newline */
        line[strcspn(line, "\r\n")] = '\0';
        str_trim(line);

        /* skip blanks & comments */
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        /* section header */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
                str_trim(section);
            }
            continue;
        }

        /* key = value */
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char key[128], val[MAX_VALUE_LEN];
        strncpy(key, line, sizeof(key) - 1);   key[sizeof(key) - 1] = '\0';
        strncpy(val, eq + 1, sizeof(val) - 1);  val[sizeof(val) - 1] = '\0';
        str_trim(key);
        str_trim(val);

        /* ── [smtp] ─────────────────────────────────────────────────── */
        if (strcmp(section, "smtp") == 0) {
            if (strcmp(key, "host") == 0)
                snprintf(cfg->server.host, sizeof(cfg->server.host), "%s", val);
            else if (strcmp(key, "port") == 0)
                cfg->server.port = atoi(val);
            else if (strcmp(key, "use_ssl") == 0)
                cfg->server.use_ssl = str_to_bool(val);
            else if (strcmp(key, "username") == 0)
                snprintf(cfg->server.username, sizeof(cfg->server.username), "%s", val);
            else if (strcmp(key, "password") == 0)
                snprintf(cfg->server.password, sizeof(cfg->server.password), "%s", val);
            else if (strcmp(key, "mail_from") == 0)
                snprintf(cfg->server.mail_from, sizeof(cfg->server.mail_from), "%s", val);
        }

        /* ── [recipients] ───────────────────────────────────────────── */
        else if (strcmp(section, "recipients") == 0) {
            if (strncmp(key, "recipient", 9) == 0 && val[0] != '\0') {
                if (cfg->recipient_count < MAX_RECIPIENTS) {
                    snprintf(cfg->recipients[cfg->recipient_count],
                             sizeof(cfg->recipients[0]), "%s", val);
                    cfg->recipient_count++;
                }
            }
        }

        /* ── [email] ────────────────────────────────────────────────── */
        else if (strcmp(section, "email") == 0) {
            if (strcmp(key, "subject") == 0)
                snprintf(cfg->subject, sizeof(cfg->subject), "%s", val);
        }

        /* ── [storage] ──────────────────────────────────────────────── */
        else if (strcmp(section, "storage") == 0) {
            if (strcmp(key, "snapshots_dir") == 0)
                snprintf(cfg->snapshots_dir, sizeof(cfg->snapshots_dir), "%s", val);
            else if (strcmp(key, "sent_archive") == 0)
                snprintf(cfg->sent_archive, sizeof(cfg->sent_archive), "%s", val);
        }

        /* ── [limits] ───────────────────────────────────────────────── */
        else if (strcmp(section, "limits") == 0) {
            if (strcmp(key, "max_attach_bytes") == 0)
                cfg->max_attach_bytes = atol(val);
            else if (strcmp(key, "max_per_email") == 0)
                cfg->max_per_email = atoi(val);
        }

        /* ── [alerts] ───────────────────────────────────────────────── */
        else if (strcmp(section, "alerts") == 0) {
            if (strcmp(key, "intrusion_enabled") == 0)
                cfg->intrusion_enabled = str_to_bool(val);
            else if (strcmp(key, "intrusion_priority") == 0)
                snprintf(cfg->intrusion_priority, sizeof(cfg->intrusion_priority), "%s", val);
            else if (strcmp(key, "intrusion_cooldown") == 0)
                cfg->intrusion_cooldown = atoi(val);
            else if (strcmp(key, "loitering_enabled") == 0)
                cfg->loitering_enabled = str_to_bool(val);
            else if (strcmp(key, "loitering_priority") == 0)
                snprintf(cfg->loitering_priority, sizeof(cfg->loitering_priority), "%s", val);
            else if (strcmp(key, "loitering_cooldown") == 0)
                cfg->loitering_cooldown = atoi(val);
            else if (strcmp(key, "motion_enabled") == 0)
                cfg->motion_enabled = str_to_bool(val);
            else if (strcmp(key, "motion_priority") == 0)
                snprintf(cfg->motion_priority, sizeof(cfg->motion_priority), "%s", val);
            else if (strcmp(key, "motion_cooldown") == 0)
                cfg->motion_cooldown = atoi(val);
        }
    }

    fclose(fp);

    /* If mail_from is empty, default to username */
    if (cfg->server.mail_from[0] == '\0' && cfg->server.username[0] != '\0')
        snprintf(cfg->server.mail_from, sizeof(cfg->server.mail_from), "%s", cfg->server.username);

    LOG_INFO("Loaded config: %s", config_path);

    /* Validate required fields */
    int ok = 1;
    if (cfg->server.host[0] == '\0')     { LOG_ERROR("Missing [smtp] host");       ok = 0; }
    if (cfg->server.username[0] == '\0') { LOG_ERROR("Missing [smtp] username");   ok = 0; }
    if (cfg->server.password[0] == '\0') { LOG_ERROR("Missing [smtp] password");   ok = 0; }
    if (cfg->recipient_count == 0){ LOG_ERROR("Missing [recipients] recipient1"); ok = 0; }

    return ok ? 0 : -1;
}

/* ── Batch splitting ───────────────────────────────────────────────────── */

typedef struct {
    const char **paths;
    int          count;
} ImageBatch;

/**
 * Split snapshot_paths into batches that fit within max_attach_bytes
 * and max_per_email.  Caller must free batches[].paths (not the strings).
 * Returns number of batches written.  max_batches is the capacity of the
 * output array.
 */
static int chunk_by_size(const char **paths, int count,
                         long max_attach_bytes, int max_per_email,
                         ImageBatch *batches, int max_batches)
{
    int nb = 0;
    int bi = 0;           /* index into current batch */
    long batch_size = 0;

    /* temporary per-batch array (reused via pointer arithmetic) */
    const char **cur = (const char **)malloc(count * sizeof(char *));
    if (!cur) return 0;

    for (int i = 0; i < count; i++) {
        struct stat st;
        long fsize = 0;
        if (stat(paths[i], &st) == 0) fsize = (long)st.st_size;

        if (bi > 0 && (batch_size + fsize > max_attach_bytes ||
                       bi >= max_per_email)) {
            /* Close current batch */
            if (nb < max_batches) {
                batches[nb].paths = (const char **)malloc(bi * sizeof(char *));
                memcpy(batches[nb].paths, cur, bi * sizeof(char *));
                batches[nb].count = bi;
                nb++;
            }
            bi = 0;
            batch_size = 0;
        }
        cur[bi++] = paths[i];
        batch_size += fsize;
    }
    /* last batch */
    if (bi > 0 && nb < max_batches) {
        batches[nb].paths = (const char **)malloc(bi * sizeof(char *));
        memcpy(batches[nb].paths, cur, bi * sizeof(char *));
        batches[nb].count = bi;
        nb++;
    }

    free(cur);
    return nb;
}

/* ── Email sending via libcurl ─────────────────────────────────────────── */

/**
 * Build the "To:" header value from all recipients.
 */
static void build_to_list(const SmtpConfig *cfg, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    for (int i = 0; i < cfg->recipient_count; i++) {
        if (i > 0) strncat(buf, ", ", bufsz - strlen(buf) - 1);
        strncat(buf, cfg->recipients[i], bufsz - strlen(buf) - 1);
    }
}

/**
 * Build the plain-text body of the email.
 */
static char *build_body(const ImageBatch *batch,
                        int batch_num, int total_batches,
                        int total_images, const SmtpConfig *cfg,
                        const char *alert_type)
{
    /* generous buffer */
    size_t cap = 2048 + batch->count * MAX_PATH_LEN;
    char *body = (char *)malloc(cap);
    if (!body) return NULL;
    body[0] = '\0';

    char nowbuf[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(nowbuf, sizeof(nowbuf), "%Y-%m-%d %H:%M:%S", &tm);

    char tmp[MAX_PATH_LEN + 128];

    if (alert_type && alert_type[0]) {
        /* create upper-case copy */
        char upper[64];
        strncpy(upper, alert_type, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        for (char *p = upper; *p; p++) *p = toupper((unsigned char)*p);
        snprintf(tmp, sizeof(tmp), "AI Camera -- %s Snapshot Alert\n", upper);
    } else {
        snprintf(tmp, sizeof(tmp), "AI Camera Snapshot Alert\n");
    }
    strncat(body, tmp, cap - strlen(body) - 1);
    strncat(body, "========================================\n", cap - strlen(body) - 1);

    if (alert_type && alert_type[0]) {
        char upper[64];
        strncpy(upper, alert_type, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        for (char *p = upper; *p; p++) *p = toupper((unsigned char)*p);
        snprintf(tmp, sizeof(tmp), "Alert type     : %s\n", upper);
        strncat(body, tmp, cap - strlen(body) - 1);
    }

    snprintf(tmp, sizeof(tmp), "Sent at        : %s\n", nowbuf);
    strncat(body, tmp, cap - strlen(body) - 1);
    snprintf(tmp, sizeof(tmp), "Snapshot dir   : %s\n", cfg->snapshots_dir);
    strncat(body, tmp, cap - strlen(body) - 1);
    snprintf(tmp, sizeof(tmp), "Total images   : %d\n", total_images);
    strncat(body, tmp, cap - strlen(body) - 1);

    if (total_batches > 1)
        snprintf(tmp, sizeof(tmp), "This batch     : %d image(s) (part %d of %d)\n",
                 batch->count, batch_num, total_batches);
    else
        snprintf(tmp, sizeof(tmp), "This batch     : %d image(s)\n", batch->count);
    strncat(body, tmp, cap - strlen(body) - 1);

    strncat(body, "\nAttached images:\n", cap - strlen(body) - 1);
    for (int i = 0; i < batch->count; i++) {
        snprintf(tmp, sizeof(tmp), "  * %s\n", basename_const(batch->paths[i]));
        strncat(body, tmp, cap - strlen(body) - 1);
    }
    strncat(body, "\n-- Automated alert | AI Camera System | IMX8M Plus\n",
            cap - strlen(body) - 1);
    return body;
}

/**
 * Build the email subject line.
 */
static void build_subject(char *buf, size_t bufsz,
                           const SmtpConfig *cfg,
                           const char *alert_type,
                           int batch_num, int total_batches)
{
    char upper[64] = "";
    if (alert_type && alert_type[0]) {
        strncpy(upper, alert_type, sizeof(upper) - 1);
        for (char *p = upper; *p; p++) *p = toupper((unsigned char)*p);
    }

    const char *base = (alert_type && alert_type[0])
        ? NULL   /* build dynamic */
        : cfg->subject;

    if (base) {
        if (total_batches > 1) {
            int ret = snprintf(buf, bufsz, "%s [%d/%d]", base, batch_num, total_batches);
            (void)ret; /* allow truncation */
        } else {
            snprintf(buf, bufsz, "%s", base);
    }} else {
        if (total_batches > 1)
            snprintf(buf, bufsz, "AI Camera - %s Alert [%d/%d]",
                     upper, batch_num, total_batches);
        else
            snprintf(buf, bufsz, "AI Camera - %s Alert", upper);
    }
}

/**
 * Send one email batch via libcurl.  Returns true on success.
 */
static bool send_one_email(const ImageBatch *batch,
                           int batch_num, int total_batches,
                           int total_images, const SmtpConfig *cfg,
                           const char *alert_type,
                           int max_retries, bool dry_run)
{
    /* Build subject */
    char subject[MAX_SUBJECT_LEN];
    build_subject(subject, sizeof(subject), cfg, alert_type,
                  batch_num, total_batches);

    if (dry_run) {
        LOG_INFO("[DRY RUN] Would send email:");
        LOG_INFO("  From    : %s", cfg->server.mail_from);
        char to[1024]; build_to_list(cfg, to, sizeof(to));
        LOG_INFO("  To      : %s", to);
        LOG_INFO("  Subject : %s", subject);
        for (int i = 0; i < batch->count; i++)
            LOG_INFO("  Attach  : %s", basename_const(batch->paths[i]));
        return true;
    }

    LOG_INFO("Connecting to %s:%d (SSL=%s) ...",
             cfg->server.host, cfg->server.port, cfg->server.use_ssl ? "true" : "false");

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            LOG_ERROR("curl_easy_init() failed");
            return false;
        }

        /* SMTP URL */
        char url[512];
        if (cfg->server.use_ssl)
            snprintf(url, sizeof(url), "smtps://%s:%d", cfg->server.host, cfg->server.port);
        else
            snprintf(url, sizeof(url), "smtp://%s:%d", cfg->server.host, cfg->server.port);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg->server.username);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg->server.password);

        if (!cfg->server.use_ssl) {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
        }

        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, cfg->server.mail_from);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

        /* Recipients */
        struct curl_slist *rcpts = NULL;
        for (int i = 0; i < cfg->recipient_count; i++)
            rcpts = curl_slist_append(rcpts, cfg->recipients[i]);
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpts);

        /* Build MIME message */
        curl_mime *mime = curl_mime_init(curl);

        /* Headers part (From, To, Subject) */
        struct curl_slist *headers = NULL;
        char hdr[2048];
        snprintf(hdr, sizeof(hdr), "From: %s", cfg->server.mail_from);
        headers = curl_slist_append(headers, hdr);
        char to[1024]; build_to_list(cfg, to, sizeof(to));
        snprintf(hdr, sizeof(hdr), "To: %s", to);
        headers = curl_slist_append(headers, hdr);
        snprintf(hdr, sizeof(hdr), "Subject: %s", subject);
        headers = curl_slist_append(headers, hdr);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        /* Text body part */
        char *body = build_body(batch, batch_num, total_batches,
                                total_images, cfg, alert_type);
        if (body) {
            curl_mimepart *part = curl_mime_addpart(mime);
            curl_mime_data(part, body, CURL_ZERO_TERMINATED);
            curl_mime_type(part, "text/plain; charset=UTF-8");
        }

        /* Attachment parts */
        for (int i = 0; i < batch->count; i++) {
            curl_mimepart *part = curl_mime_addpart(mime);
            curl_mime_filedata(part, batch->paths[i]);
            /* Determine MIME type from extension */
            const char *dot = strrchr(batch->paths[i], '.');
            if (dot && strcasecmp(dot, ".png") == 0)
                curl_mime_type(part, "image/png");
            else
                curl_mime_type(part, "image/jpeg");
            curl_mime_filename(part, basename_const(batch->paths[i]));
            curl_mime_encoder(part, "base64");
        }

        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

        /* Send */
        CURLcode res = curl_easy_perform(curl);

        /* Cleanup */
        free(body);
        curl_slist_free_all(rcpts);
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK) {
            LOG_INFO("Email sent successfully to: %s", to);
            return true;
        }

        /* Check for auth failure — no retry */
        if (res == CURLE_LOGIN_DENIED) {
            LOG_ERROR("Authentication failed -- check [smtp] username / password in config.");
            return false;
        }

        int wait = 1 << attempt;   /* 2, 4, 8 ... */
        LOG_WARN("SMTP error (attempt %d/%d): %s -- retrying in %ds",
                 attempt, max_retries, curl_easy_strerror(res), wait);
        if (attempt < max_retries) sleep(wait);
    }

    LOG_ERROR("Failed to send email after %d attempts.", max_retries);
    return false;
}

/* ── Archive / delete ──────────────────────────────────────────────────── */

void archive_or_delete(const char **paths, int count,
                       const char *sent_archive)
{
    if (!sent_archive || sent_archive[0] == '\0') {
        /* Delete */
        for (int i = 0; i < count; i++) {
            if (unlink(paths[i]) == 0)
                LOG_INFO("Deleted: %s", basename_const(paths[i]));
            else
                LOG_WARN("Could not delete %s: %s",
                         basename_const(paths[i]), strerror(errno));
        }
        return;
    }

    /* Create archive directory (best effort) */
    mkdir(sent_archive, 0755);

    for (int i = 0; i < count; i++) {
        const char *name = basename_const(paths[i]);
        char dest[MAX_PATH_LEN];
        snprintf(dest, sizeof(dest), "%s/%s", sent_archive, name);

        /* If destination exists, append timestamp */
        struct stat st;
        if (stat(dest, &st) == 0) {
            /* strip extension, append timestamp, re-add extension */
            const char *dot = strrchr(name, '.');
            if (dot) {
                size_t stem_len = (size_t)(dot - name);
                char stem[256];
                snprintf(stem, sizeof(stem), "%.*s", (int)stem_len, name);
                snprintf(dest, sizeof(dest), "%s/%s_%ld%s",
                         sent_archive, stem, (long)time(NULL), dot);
            }
        }

        if (rename(paths[i], dest) == 0)
            LOG_INFO("Archived: %s -> %s", name, dest);
        else
            LOG_WARN("Could not archive %s: %s", name, strerror(errno));
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool send_snapshots(const char **snapshot_paths, int count,
                    const char *alert_type, const SmtpConfig *cfg,
                    bool dry_run, bool archive)
{
    /* Filter to existing files */
    const char *valid[MAX_SNAPSHOTS];
    int nvalid = 0;
    for (int i = 0; i < count && nvalid < MAX_SNAPSHOTS; i++) {
        struct stat st;
        if (stat(snapshot_paths[i], &st) == 0 && S_ISREG(st.st_mode))
            valid[nvalid++] = snapshot_paths[i];
    }

    if (nvalid == 0) {
        LOG_WARN("send_snapshots called with no valid image paths.");
        return false;
    }

    LOG_INFO("send_snapshots: alert_type=%s, %d image(s), dry_run=%s",
             alert_type ? alert_type : "(none)", nvalid,
             dry_run ? "true" : "false");

    /* Split into batches */
    ImageBatch batches[64];
    int nbatches = chunk_by_size(valid, nvalid,
                                  cfg->max_attach_bytes, cfg->max_per_email,
                                  batches, 64);

    bool all_ok = true;
    const char *sent[MAX_SNAPSHOTS];
    int nsent = 0;

    for (int i = 0; i < nbatches; i++) {
        LOG_INFO("Batch %d/%d -- %d image(s)", i + 1, nbatches, batches[i].count);
        bool ok = send_one_email(&batches[i], i + 1, nbatches, nvalid,
                                  cfg, alert_type, 3, dry_run);
        if (ok) {
            for (int j = 0; j < batches[i].count && nsent < MAX_SNAPSHOTS; j++)
                sent[nsent++] = batches[i].paths[j];
        } else {
            all_ok = false;
            LOG_ERROR("Batch %d/%d failed -- images kept for retry.",
                      i + 1, nbatches);
        }
    }

    if (archive && nsent > 0 && !dry_run) {
        archive_or_delete(sent, nsent, cfg->sent_archive);
        LOG_INFO("Archived/deleted %d sent image(s).", nsent);
    }

    /* Free batch arrays */
    for (int i = 0; i < nbatches; i++)
        free(batches[i].paths);

    return all_ok;
}

/* ── OTP / Authentication ───────────────────────────────────────────────── */

#define APP_NAME "SmartCamera"

/*
 * _smtp_send_html
 */
static bool _smtp_send_html(const SmtpServerConfig *cfg,
                            const char **recipients, int num_recipients,
                            const char *subject,
                            const char *plain_text,
                            const char *html_body,   /* NULL = plain only */
                            const char *file_path)   /* NULL = no image   */
{
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    char url[256];
    if (cfg->use_ssl)
        snprintf(url, sizeof(url), "smtps://%s:%d", cfg->host, cfg->port);
    else
        snprintf(url, sizeof(url), "smtp://%s:%d", cfg->host, cfg->port);

    curl_easy_setopt(curl, CURLOPT_URL,       url);
    curl_easy_setopt(curl, CURLOPT_USERNAME,  cfg->username);
    curl_easy_setopt(curl, CURLOPT_PASSWORD,  cfg->password);
    if (!cfg->use_ssl) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    }
    curl_easy_setopt(curl, CURLOPT_VERBOSE,   0L);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, cfg->mail_from);

    struct curl_slist *rcpts = NULL;
    for (int i = 0; i < num_recipients; i++) {
        rcpts = curl_slist_append(rcpts, recipients[i]);
    }
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpts);

    curl_mime *mime_root = curl_mime_init(curl);

    /* ── multipart/alternative sub-part ── */
    curl_mimepart *alt_part = curl_mime_addpart(mime_root);
    curl_mime     *mime_alt = curl_mime_init(curl);

    /* plain-text leaf */
    curl_mimepart *p = curl_mime_addpart(mime_alt);
    curl_mime_data(p, plain_text, CURL_ZERO_TERMINATED);
    curl_mime_type(p, "text/plain; charset=utf-8");
    curl_mime_encoder(p, "quoted-printable");

    /* HTML leaf */
    if (html_body) {
        p = curl_mime_addpart(mime_alt);
        curl_mime_data(p, html_body, CURL_ZERO_TERMINATED);
        curl_mime_type(p, "text/html; charset=utf-8");
        curl_mime_encoder(p, "quoted-printable");
    }

    curl_mime_subparts(alt_part, mime_alt);
    curl_mime_type(alt_part, "multipart/alternative");

    /* ── inline PNG ── */
    if (file_path && file_path[0] != '\0') {
        curl_mimepart *img = curl_mime_addpart(mime_root);
        curl_mime_filedata(img, file_path);
        curl_mime_type(img, "image/png");
        curl_mime_encoder(img, "base64");
        curl_mime_filename(img, "qr.png");

        struct curl_slist *img_hdrs = NULL;
        img_hdrs = curl_slist_append(img_hdrs, "Content-ID: <qr_code_image>");
        img_hdrs = curl_slist_append(img_hdrs,
                       "Content-Disposition: inline; filename=\"qr.png\"");
        curl_mime_headers(img, img_hdrs, 1);
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime_root);

    /* RFC-2822 display headers */
    struct curl_slist *msg_hdrs = NULL;
    char hdr_from[256], hdr_subj[256];
    snprintf(hdr_from, sizeof(hdr_from), "From: %s", cfg->mail_from);
    snprintf(hdr_subj, sizeof(hdr_subj), "Subject: %s", subject);
    msg_hdrs = curl_slist_append(msg_hdrs, hdr_from);
    msg_hdrs = curl_slist_append(msg_hdrs, hdr_subj);

    /* Build "To:" from recipients array */
    char hdr_to[512] = "To: ";
    for (int i = 0; i < num_recipients; i++) {
        if (i > 0) strncat(hdr_to, ", ", sizeof(hdr_to) - strlen(hdr_to) - 1);
        strncat(hdr_to, recipients[i], sizeof(hdr_to) - strlen(hdr_to) - 1);
    }
    msg_hdrs = curl_slist_append(msg_hdrs, hdr_to);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, msg_hdrs);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(rcpts);
    curl_slist_free_all(msg_hdrs);
    curl_mime_free(mime_root);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR("[EmailOTP] FAILED error=%s", curl_easy_strerror(res));
        return false;
    }
    return true;
}

bool email_otp_send(const SmtpServerConfig *cfg,
                    const char **recipients, int num_recipients,
                    const char *otp)
{
    char subject[128];
    snprintf(subject, sizeof(subject), "[%s] Your One-Time Password", APP_NAME);

    char body[512];
    snprintf(body, sizeof(body),
             "Hello,\r\n\r\n"
             "Your One-Time Password for %s is:\r\n\r\n"
             "    %s\r\n\r\n"
             "This code is valid for 5 minutes. "
             "Do not share it with anyone.\r\n\r\n"
             "If you did not request this, please ignore this email.\r\n\r\n"
             "-- %s Security\r\n",
             APP_NAME, otp, APP_NAME);

    bool ok = _smtp_send_html(cfg, recipients, num_recipients, subject, body, NULL, NULL);
    if (ok) {
        for (int i = 0; i < num_recipients; i++) {
            LOG_INFO("[EmailOTP] sent to=%s  otp=%s", recipients[i], otp);
        }
    }
    return ok;
}

bool email_otp_send_totp_setup(const SmtpServerConfig *cfg,
                               const char **recipients, int num_recipients,
                               const char *username,
                               const char *qr_path,
                               const char *uri)
{
    /* Extract base32 secret from URI */
    char secret_part[100] = "";
    const char *q = strchr(uri, '?');
    if (q) {
        const char *p = strstr(q, "secret=");
        if (p) {
            p += 7;
            int i = 0;
            while (*p && *p != '&' && i < 99)
                secret_part[i++] = *p++;
            secret_part[i] = '\0';
        }
    }

    char subject[128];
    snprintf(subject, sizeof(subject),
             "[%s] Set Up Microsoft Authenticator", APP_NAME);

    /* Plain-text fallback */
    char plain[1024];
    snprintf(plain, sizeof(plain),
             "Hi %s,\r\n\r\n"
             "Your account has been configured for two-factor authentication\r\n"
             "using Microsoft Authenticator. Once set up, the app will generate\r\n"
             "a fresh 6-digit code every 30 seconds — no internet needed.\r\n\r\n"
             "Setup instructions:\r\n"
             "1. Install Microsoft Authenticator from the App Store or Google Play.\r\n"
             "2. Tap + > Other account (Google, Facebook, etc.)\r\n"
             "3. Choose Scan a QR code and point your camera at the attached image.\r\n"
             "4. The account %s / %s will appear in the app.\r\n\r\n"
             "Can't scan? Enter manually:\r\n"
             "  Account name : %s\r\n"
             "  Secret key   : %s\r\n"
             "  Type         : Time-based (TOTP)\r\n\r\n"
             "KEEP THIS EMAIL PRIVATE. Delete after setup.\r\n"
             "If you did not request this, contact your administrator.\r\n\r\n"
             "-- %s (automated message, do not reply)\r\n",
             username, APP_NAME, username, username, secret_part, APP_NAME);

    /* HTML body */
    char html[4096];
    snprintf(html, sizeof(html),
        "<html><body style=\"font-family:Arial,sans-serif;color:#222;"
                             "max-width:560px;margin:auto;\">\r\n"
        "  <h2 style=\"color:#0078d4;\">&#128737; Set Up Microsoft Authenticator"
                     " for %s</h2>\r\n"
        "  <p>Hi <strong>%s</strong>,</p>\r\n"
        "  <p>Your account has been configured for two-factor authentication using\r\n"
        "     <strong>Microsoft Authenticator</strong>. Once set up, the app will\r\n"
        "     generate a fresh 6-digit code every 30 seconds &mdash; no internet"
                     " needed.</p>\r\n"
        "  <hr style=\"border:none;border-top:1px solid #ddd;margin:16px 0;\">\r\n"
        "  <h3>Setup instructions</h3>\r\n"
        "  <ol style=\"line-height:1.8;\">\r\n"
        "    <li>Install <strong>Microsoft Authenticator</strong> from the App"
                       " Store or Google Play.</li>\r\n"
        "    <li>Tap <strong>+</strong> &rarr; <em>Other account (Google,"
                       " Facebook, etc.)</em></li>\r\n"
        "    <li>Choose <strong>Scan a QR code</strong> and point your camera"
                       " at the image below.</li>\r\n"
        "    <li>The account <em>%s / %s</em> will appear in the app.</li>\r\n"
        "    <li>At your next login, choose <strong>Microsoft Authenticator</strong>"
                       " and enter the 6-digit code displayed in the app.</li>\r\n"
        "  </ol>\r\n"
        "  <div style=\"text-align:center;margin:24px 0;\">\r\n"
        "    <img src=\"cid:qr_code_image\""
               " alt=\"Scan this QR code in Microsoft Authenticator\"\r\n"
        "         style=\"width:200px;height:200px;border:1px solid #ccc;"
                         "padding:10px;border-radius:8px;\">\r\n"
        "    <p style=\"font-size:0.8em;color:#888;margin-top:6px;\">"
               "Scan with Microsoft Authenticator</p>\r\n"
        "  </div>\r\n"
        "  <div style=\"background:#f0f4ff;padding:14px 18px;border-radius:8px;\r\n"
        "              border-left:4px solid #0078d4;margin:16px 0;\">\r\n"
        "    <strong>Can't scan the QR code?</strong> Enter these details manually"
                     " in the app:<br><br>\r\n"
        "    <table style=\"font-size:0.95em;\">\r\n"
        "      <tr><td style=\"color:#555;padding-right:12px;\">Account name</td>\r\n"
        "          <td><strong>%s</strong></td></tr>\r\n"
        "      <tr><td style=\"color:#555;padding-right:12px;\">Secret key</td>\r\n"
        "          <td><code style=\"font-size:1.1em;letter-spacing:2px;"
                              "color:#0078d4;\">%s</code></td></tr>\r\n"
        "      <tr><td style=\"color:#555;padding-right:12px;\">Type</td>\r\n"
        "          <td>Time-based (TOTP)</td></tr>\r\n"
        "    </table>\r\n"
        "  </div>\r\n"
        "  <p style=\"background:#fff3cd;padding:12px;border-radius:6px;\r\n"
        "            border-left:4px solid #ffc107;font-size:0.9em;\">\r\n"
        "    &#9888; <strong>Keep this email private.</strong>\r\n"
        "    Anyone with this QR code or secret key can generate your login codes.\r\n"
        "    Delete this email after setup.\r\n"
        "  </p>\r\n"
        "  <hr style=\"border:none;border-top:1px solid #ddd;margin:20px 0;\">\r\n"
        "  <p style=\"font-size:0.82em;color:#999;\">\r\n"
        "    If you did not request this setup, contact your administrator"
                     " immediately.<br>\r\n"
        "    This is an automated message from %s &mdash; do not reply.\r\n"
        "  </p>\r\n"
        "</body></html>\r\n",
        APP_NAME, username,          /* h2 + hi */
        APP_NAME, username,          /* step 4 */
        username, secret_part,       /* manual entry table */
        APP_NAME);                   /* footer */

    bool ok = _smtp_send_html(cfg, recipients, num_recipients, subject, plain, html, qr_path);
    if (ok) {
        for (int i = 0; i < num_recipients; i++) {
            LOG_INFO("[EmailOTP] TOTP setup email sent  to=%s  user=%s  qr=%s",
                   recipients[i], username, qr_path);
        }
    } else {
        for (int i = 0; i < num_recipients; i++) {
            LOG_ERROR("[EmailOTP] TOTP setup email FAILED  to=%s", recipients[i]);
        }
    }
    return ok;
}
