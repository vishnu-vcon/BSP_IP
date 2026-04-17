#ifndef PASSWORD_HASH_H
#define PASSWORD_HASH_H

#include <stdbool.h>

/* bcrypt hash length is always 60 characters + null terminator */
#define BCRYPT_HASH_LEN 61

/* Hash a plaintext password using bcrypt.
 * out must be at least BCRYPT_HASH_LEN bytes.
 * Returns true on success. */
bool hash_password(const char *password, char out[BCRYPT_HASH_LEN]);

/* Verify plaintext password against stored bcrypt hash.
 * Returns true if they match. */
bool verify_password(const char *password, const char *stored_hash);

#endif /* PASSWORD_HASH_H */
