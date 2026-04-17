#ifndef DEVICE_CONTROLS_H
#define DEVICE_CONTROLS_H

#include "auth/session_manager.h"
#include "auth/activity_logger.h"

/* All functions take the active session, client IP string, and
 * write a JSON result into result_buf (size result_buf_size).
 * Return 0 on success, -1 on error. */

int device_reboot  (const Session *session, const char *ip,
                    char *result_buf, size_t result_buf_size);

int device_status  (const Session *session, const char *ip,
                    char *result_buf, size_t result_buf_size);

int network_reset  (const Session *session, const char *ip,
                    char *result_buf, size_t result_buf_size);

int config_reset   (const Session *session, const char *ip,
                    char *result_buf, size_t result_buf_size);

/* factory_reset also takes the confirm flag parsed from request body.
 * Returns -2 if confirm == false (caller returns HTTP 400). */
int factory_reset  (const Session *session, const char *ip,
                    int confirm,
                    char *result_buf, size_t result_buf_size);

#endif /* DEVICE_CONTROLS_H */
