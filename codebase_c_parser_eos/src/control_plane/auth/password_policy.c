/*
 * auth/password_policy.c
 *
 * Password complexity rules:
 *   - Min 8 characters
 *   - At least 1 uppercase (A-Z)
 *   - At least 1 lowercase (a-z)
 *   - At least 1 digit (0-9)
 *   - At least 1 special character (@#$%^&*!)
 */

#include "password_policy.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

bool password_policy_check(const char *password,
                           char errors[][PASSWORD_ERROR_LEN],
                           int *error_count,
                           int max_errors)
{
    *error_count = 0;
    if (!password) {
        if (max_errors > 0) {
            snprintf(errors[(*error_count)++], PASSWORD_ERROR_LEN,
                     "Password must not be NULL");
        }
        return false;
    }

    size_t len = strlen(password);
    bool has_upper   = false;
    bool has_lower   = false;
    bool has_digit   = false;
    bool has_special = false;
    const char *specials = "@#$%^&*!";

    for (size_t i = 0; i < len; i++) {
        char c = password[i];
        if (isupper((unsigned char)c)) has_upper   = true;
        if (islower((unsigned char)c)) has_lower   = true;
        if (isdigit((unsigned char)c)) has_digit   = true;
        if (strchr(specials, c))        has_special = true;
    }

#define ADD_ERROR(msg) \
    do { \
        if (*error_count < max_errors) \
            snprintf(errors[(*error_count)++], PASSWORD_ERROR_LEN, "%s", (msg)); \
    } while (0)

    if (len < 8)
        ADD_ERROR("Password must be at least 8 characters");
    if (!has_upper)
        ADD_ERROR("Password must contain at least 1 uppercase letter (A-Z)");
    if (!has_lower)
        ADD_ERROR("Password must contain at least 1 lowercase letter (a-z)");
    if (!has_digit)
        ADD_ERROR("Password must contain at least 1 number (0-9)");
    if (!has_special)
        ADD_ERROR("Password must contain at least 1 special character (@#$%%^&*!)");

#undef ADD_ERROR

    if (*error_count > 0) {
        printf("  [Policy] FAILED: %s\n", errors[0]);
        return false;
    }
    printf("  [Policy] PASSED all 5 rules\n");
    return true;
}
