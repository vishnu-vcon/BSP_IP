#ifndef PASSWORD_POLICY_H
#define PASSWORD_POLICY_H

#include <stdbool.h>

#define PASSWORD_MAX_ERRORS 10
#define PASSWORD_ERROR_LEN  128

/* Check password against policy rules.
 * Returns true if all rules pass.
 * errors[] is filled with up to max_errors null-terminated strings.
 * error_count is set to the number of errors found. */
bool password_policy_check(const char *password,
                           char errors[][PASSWORD_ERROR_LEN],
                           int *error_count,
                           int max_errors);

#endif /* PASSWORD_POLICY_H */
