/*
 * auth/password_hash.c
 *
 * bcrypt password hashing.
 * Hash is stored as a 60-character string (+ null).
 *
 * Requires linking with -lcrypt  (glibc crypt library).
 * On Debian/Ubuntu:  sudo apt-get install libxcrypt-dev
 * Compile:           gcc ... -lcrypt
 *
 * Note: glibc's crypt() supports bcrypt with the "$2b$" prefix.
 */

#include "password_hash.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* glibc crypt/crypt.h */
#define _GNU_SOURCE
#include <crypt.h>
#include <time.h>

/* Build a bcrypt salt string: "$2b$12$" + 22 random chars from the
 * base-64 alphabet used by crypt(). */
static void make_bcrypt_salt(char salt_out[32])
{
    static const char b64[] =
        "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    srand((unsigned)time(NULL));
    char *p = salt_out;
    p += sprintf(p, "$2b$12$");
    for (int i = 0; i < 22; i++)
        *p++ = b64[rand() % 64];
    *p = '\0';
}

bool hash_password(const char *password, char out[BCRYPT_HASH_LEN])
{
    char salt[32];
    make_bcrypt_salt(salt);

    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *hash = crypt_r(password, salt, &data);
    if (!hash) {
        fprintf(stderr, "  [HashPW] crypt_r failed\n");
        return false;
    }
    strncpy(out, hash, BCRYPT_HASH_LEN - 1);
    out[BCRYPT_HASH_LEN - 1] = '\0';
    return true;
}

bool verify_password(const char *password, const char *stored_hash)
{
    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *hash = crypt_r(password, stored_hash, &data);
    if (!hash) return false;
    return strcmp(hash, stored_hash) == 0;
}
