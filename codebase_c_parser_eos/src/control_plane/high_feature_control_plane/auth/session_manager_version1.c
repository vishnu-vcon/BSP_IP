/*
 * auth/session_manager.c
 *
 * Token-based session management.
 *
 * Tokens expire after TOKEN_TTL_SEC (2 hours).
 * Pending 2-FA sessions expire after OTP_TTL_SEC (5 minutes).
 *
 * Tokens are 64 random hex characters generated from /dev/urandom.
 */

#include "session_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* ---------- In-memory stores ---------- */
static Session        _sessions[MAX_SESSIONS];
static PendingSession _pending[MAX_PENDING];

/* Single mutex protects both _sessions and _pending arrays.
 * All threads that call create/validate/delete go through this lock. */
static pthread_mutex_t _sess_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Token generation ---------- */
static bool generate_token(char out[TOKEN_LEN])
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) {
        perror("  [Session] cannot open /dev/urandom");
        return false;
    }
    unsigned char buf[32];
    if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        fclose(f);
        return false;
    }
    fclose(f);
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", buf[i]);
    out[64] = '\0';
    return true;
}

/* ---------- Active sessions ---------- */

bool create_session(const char *username, const char *role, char out[TOKEN_LEN])
{
    if (!generate_token(out)) return false;

    pthread_mutex_lock(&_sess_mutex);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!_sessions[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&_sess_mutex);
        fprintf(stderr, "  [Session] session table full\n");
        return false;
    }

    Session *s = &_sessions[slot];
    strncpy(s->token, out, TOKEN_LEN - 1);
    strncpy(s->user,  username, USERNAME_MAX - 1);
    strncpy(s->role,  role,     ROLE_MAX - 1);
    s->expires_at = time(NULL) + TOKEN_TTL_SEC;
    s->active     = true;

    pthread_mutex_unlock(&_sess_mutex);

    printf("  [Session] token issued  user=%s  role=%s  ttl=%ds\n",
           username, role, TOKEN_TTL_SEC);
    return true;
}

bool validate_session(const char *token, Session *sess)
{
    pthread_mutex_lock(&_sess_mutex);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &_sessions[i];
        if (!s->active) continue;
        if (strcmp(s->token, token) != 0) continue;

        if (time(NULL) > s->expires_at) {
            s->active = false;
            pthread_mutex_unlock(&_sess_mutex);
            printf("  [Session] token expired\n");
            return false;
        }
        if (sess) *sess = *s;
        pthread_mutex_unlock(&_sess_mutex);
        printf("  [Session] valid  user=%s  role=%s\n", s->user, s->role);
        return true;
    }
    pthread_mutex_unlock(&_sess_mutex);
    printf("  [Session] token not found\n");
    return false;
}

void delete_session(const char *token)
{
    pthread_mutex_lock(&_sess_mutex);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (_sessions[i].active && strcmp(_sessions[i].token, token) == 0) {
            _sessions[i].active = false;
            pthread_mutex_unlock(&_sess_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&_sess_mutex);
}

/* ---------- Pending (2-FA waiting) sessions ---------- */

void pending_set(const char *username, const PendingSession *data)
{
    pthread_mutex_lock(&_sess_mutex);
    /* Update existing or find free slot */
    for (int i = 0; i < MAX_PENDING; i++) {
        if (_pending[i].active &&
            strcmp(_pending[i].username, username) == 0) {
            _pending[i] = *data;
            strncpy(_pending[i].username, username, USERNAME_MAX - 1);
            _pending[i].active = true;
            if (_pending[i].expires_at == 0)
                _pending[i].expires_at = time(NULL) + OTP_TTL_SEC;
            pthread_mutex_unlock(&_sess_mutex);
            return;
        }
    }
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!_pending[i].active) {
            _pending[i] = *data;
            strncpy(_pending[i].username, username, USERNAME_MAX - 1);
            _pending[i].active = true;
            if (_pending[i].expires_at == 0)
                _pending[i].expires_at = time(NULL) + OTP_TTL_SEC;
            pthread_mutex_unlock(&_sess_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&_sess_mutex);
    fprintf(stderr, "  [Session] pending table full\n");
}

bool pending_get(const char *username, PendingSession *out)
{
    pthread_mutex_lock(&_sess_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
        PendingSession *p = &_pending[i];
        if (!p->active) continue;
        if (strcmp(p->username, username) != 0) continue;

        if (time(NULL) > p->expires_at) {
            p->active = false;
            pthread_mutex_unlock(&_sess_mutex);
            printf("  [Session] pending expired  user=%s\n", username);
            return false;
        }
        if (out) *out = *p;
        pthread_mutex_unlock(&_sess_mutex);
        return true;
    }
    pthread_mutex_unlock(&_sess_mutex);
    return false;
}

void pending_delete(const char *username)
{
    pthread_mutex_lock(&_sess_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (_pending[i].active &&
            strcmp(_pending[i].username, username) == 0) {
            _pending[i].active = false;
            pthread_mutex_unlock(&_sess_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&_sess_mutex);
}
