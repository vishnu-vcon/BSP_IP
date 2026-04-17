#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <stdbool.h>
#include <time.h>

#define TOKEN_LEN       65   /* 64 hex chars + null */
#define USERNAME_MAX    64
#define ROLE_MAX        32
#define ACTION_MAX      32
#define OTP_MAX         16
#define TOKEN_TTL_SEC   7200   /* 2 hours */
#define OTP_TTL_SEC     300    /* 5 minutes */
#define MAX_SESSIONS    256
#define MAX_PENDING     64

/* ---------- Active session ---------- */
typedef struct {
    char   token[TOKEN_LEN];
    char   user[USERNAME_MAX];
    char   role[ROLE_MAX];
    time_t expires_at;
    bool   active;
} Session;

/* ---------- Pending 2-FA state ---------- */
typedef struct {
    char   username[USERNAME_MAX];
    char   role[ROLE_MAX];
    char   otp[OTP_MAX];
    char   action[ACTION_MAX];   /* "reset_password" or "" */
    time_t expires_at;
    bool   otp_verified;
    bool   totp_verified;
    bool   active;
} PendingSession;

/* Create session; token written to out (must be TOKEN_LEN bytes).
 * Returns true on success. */
bool create_session(const char *username, const char *role, char out[TOKEN_LEN]);

/* Validate token; fills sess on success.  Returns true if valid. */
bool validate_session(const char *token, Session *sess);

/* Invalidate / delete a session token. */
void delete_session(const char *token);

/* Pending session management */
void   pending_set(const char *username, const PendingSession *data);
bool   pending_get(const char *username, PendingSession *out);
void   pending_delete(const char *username);

#endif /* SESSION_MANAGER_H */
