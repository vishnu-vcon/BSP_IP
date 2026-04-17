/*
 * smtp_sender.h
 * -------------
 * SMTP email service for the AI Camera pipeline.
 * Provides config loading, email building (MIME + attachments via libcurl),
 * SMTP send with retry, and sent-image archival.
 */

#ifndef SMTP_SENDER_H
#define SMTP_SENDER_H

#include <stdbool.h>

/* ── Limits ─────────────────────────────────────────────────────────────── */

#define MAX_RECIPIENTS      4
#define MAX_PATH_LEN        512
#define MAX_VALUE_LEN       256
#define MAX_SNAPSHOTS       256
#define MAX_SUBJECT_LEN     256

/* ── Config struct ──────────────────────────────────────────────────────── */

typedef struct {
    char host[MAX_VALUE_LEN];
    int  port;
    bool use_ssl;
    char username[MAX_VALUE_LEN];
    char password[MAX_VALUE_LEN];
    char mail_from[MAX_VALUE_LEN];
} SmtpServerConfig;

typedef struct {
    /* SMTP connection */
    SmtpServerConfig server;

    /* Recipients */
    char recipients[MAX_RECIPIENTS][MAX_VALUE_LEN];
    int  recipient_count;

    /* Email */
    char subject[MAX_SUBJECT_LEN];

    /* Storage */
    char snapshots_dir[MAX_PATH_LEN];
    char sent_archive[MAX_PATH_LEN];

    /* Limits */
    long max_attach_bytes;
    int  max_per_email;

    /* ── Alert rules (raw values, parsed by alert_rules module) ────────── */
    /* intrusion */
    bool intrusion_enabled;
    char intrusion_priority[MAX_VALUE_LEN];
    int  intrusion_cooldown;
    /* loitering */
    bool loitering_enabled;
    char loitering_priority[MAX_VALUE_LEN];
    int  loitering_cooldown;
    /* motion */
    bool motion_enabled;
    char motion_priority[MAX_VALUE_LEN];
    int  motion_cooldown;
} SmtpConfig;

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * Load and validate smtp.conf.  Returns 0 on success, -1 on error
 * (missing file or missing required fields).
 */
int  load_config(const char *config_path, SmtpConfig *cfg);

/**
 * Send snapshot images via email.
 *
 * @param snapshot_paths  Array of file-path strings.
 * @param count           Number of paths in the array.
 * @param alert_type      Alert category string (e.g. "intrusion").
 * @param cfg             Loaded config.
 * @param dry_run         If true, log actions but do not connect.
 * @param archive         If true, archive/delete images after send.
 * @return true on success (all batches sent), false otherwise.
 */
bool send_snapshots(const char **snapshot_paths, int count,
                    const char *alert_type, const SmtpConfig *cfg,
                    bool dry_run, bool archive);

/**
 * Move sent images to the archive directory, or delete if archive is empty.
 */
void archive_or_delete(const char **paths, int count,
                       const char *sent_archive);

/* ── OTP / Authentication ───────────────────────────────────────────────── */

/**
 * Send a 6-digit OTP via email.
 */
bool email_otp_send(const SmtpServerConfig *cfg,
                    const char **recipients, int num_recipients,
                    const char *otp);

/**
 * Send a TOTP setup email with QR code attachment.
 */
bool email_otp_send_totp_setup(const SmtpServerConfig *cfg,
                               const char **recipients, int num_recipients,
                               const char *username,
                               const char *qr_path,
                               const char *uri);

#endif /* SMTP_SENDER_H */
