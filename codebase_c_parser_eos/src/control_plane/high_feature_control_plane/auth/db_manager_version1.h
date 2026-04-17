#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#include <stdbool.h>

#define DB_FILE        "users.db"
#define HASH_FIELD_MAX  64
#define EMAIL_MAX       128
#define TOTP_MAX        64

typedef struct {
    char username     [64];
    char password_hash[HASH_FIELD_MAX];
    char role         [32];
    char email        [EMAIL_MAX];
    char totp_secret  [TOTP_MAX];
} UserRecord;

/* Initialize the SQLite users table (creates if not exists). */
void init_db(void);

/* Returns true if username exists in DB. */
bool user_exists(const char *username);

/* Insert a new user. Silently skips if username already exists. */
void insert_user(const char *username,
                 const char *password_hash,
                 const char *role,
                 const char *email,
                 const char *totp_secret);

/* Retrieve a user record. Returns true on success. */
bool get_user(const char *username, UserRecord *out);

/* Update password hash. */
void update_password_hash(const char *username, const char *new_hash);

/* Update TOTP secret. */
void update_totp_secret(const char *username, const char *secret);

#endif /* DB_MANAGER_H */
void delete_user(const char *username);
