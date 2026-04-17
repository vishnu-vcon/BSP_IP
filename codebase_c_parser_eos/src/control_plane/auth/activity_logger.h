#ifndef ACTIVITY_LOGGER_H
#define ACTIVITY_LOGGER_H

/*
 * activity_log_event() — renamed from log_event() to avoid collision with
 * log_event() declared in <report_log_writer.h>, which has a different
 * signature:  (event, source, log_severity_t, message).
 *
 * Appends a JSON line to /data/device_logs/user_activity.log AND prints a
 * colour-coded, timestamped line to stdout for real-time terminal visibility.
 *
 * ip may be NULL if not applicable.
 */
void activity_log_event(const char *user_id,
                        const char *action,
                        const char *details,
                        const char *ip);

/* Lower-level: ip must already be embedded in details if needed. */
void log_user_activity(const char *user_id,
                       const char *action,
                       const char *details);

#endif /* ACTIVITY_LOGGER_H */
