#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#include <stdbool.h>
#include "smtp_sender.h"   /* SmtpServerConfig — single canonical definition */

#define DB_FILE        "config/users.db"
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

/* Enumerate all users. */
typedef void (*UserListCallback)(const char *username, const char *role, const char *email, void *user_data);
void db_manager_list_users(UserListCallback cb, void *user_data);

/* Retrieve a user record. Returns true on success. */
bool get_user(const char *username, UserRecord *out);

/* Update password hash. */
void update_password_hash(const char *username, const char *new_hash);

/* Update TOTP secret. */
void update_totp_secret(const char *username, const char *secret);

/* Delete a user. */
void delete_user(const char *username);

/* Retrieve SMTP configuration from DB. */
bool get_smtp_config(SmtpServerConfig *out);

/* Update SMTP configuration in DB. */
void update_smtp_config(const SmtpServerConfig *in);

#endif /* DB_MANAGER_H */
