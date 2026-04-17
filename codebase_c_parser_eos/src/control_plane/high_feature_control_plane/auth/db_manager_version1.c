/*
 * auth/db_manager.c
 *
 * SQLite user store.
 * Schema: username, password_hash, role, email, totp_secret
 *
 * Key design:
 *   - Passwords are NEVER stored in plain text
 *   - Existing rows are never overwritten on INSERT
 *
 * Requires: sqlite3  (-lsqlite3)
 */

#include "db_manager.h"
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

static sqlite3 *_open_db(void)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open(DB_FILE, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  [DB] cannot open %s: %s\n",
                DB_FILE, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

void init_db(void)
{
    sqlite3 *db = _open_db();
    if (!db) return;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "  username      TEXT PRIMARY KEY,"
        "  password_hash BLOB NOT NULL,"
        "  role          TEXT NOT NULL,"
        "  email         TEXT DEFAULT '',"
        "  totp_secret   TEXT DEFAULT ''"
        ");";

    /* WAL mode: allows concurrent reads alongside a write —
     * essential because libmicrohttpd runs one thread per connection. */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "  [DB] init error: %s\n", err);
        sqlite3_free(err);
    } else {
        printf("  [DB] initialized  file=%s\n", DB_FILE);
    }
    sqlite3_close(db);
}

bool user_exists(const char *username)
{
    sqlite3 *db = _open_db();
    if (!db) return false;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM users WHERE username=?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}

void insert_user(const char *username,
                 const char *password_hash,
                 const char *role,
                 const char *email,
                 const char *totp_secret)
{
    sqlite3 *db = _open_db();
    if (!db) return;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO users VALUES (?,?,?,?,?)",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username,      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, role,          -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, email        ? email        : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, totp_secret  ? totp_secret  : "", -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        printf("  [DB] inserted user='%s'  role=%s\n", username, role);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

bool get_user(const char *username, UserRecord *out)
{
    sqlite3 *db = _open_db();
    if (!db) return false;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT username, password_hash, role, email, totp_secret "
        "FROM users WHERE username=?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        snprintf(out->username,      sizeof(out->username),
                 "%s", (const char*)sqlite3_column_text(stmt, 0));
        snprintf(out->password_hash, sizeof(out->password_hash),
                 "%s", (const char*)sqlite3_column_text(stmt, 1));
        snprintf(out->role,          sizeof(out->role),
                 "%s", (const char*)sqlite3_column_text(stmt, 2));
        snprintf(out->email,         sizeof(out->email),
                 "%s", (const char*)sqlite3_column_text(stmt, 3));
        snprintf(out->totp_secret,   sizeof(out->totp_secret),
                 "%s", (const char*)sqlite3_column_text(stmt, 4));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}

void update_password_hash(const char *username, const char *new_hash)
{
    sqlite3 *db = _open_db();
    if (!db) return;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE users SET password_hash=? WHERE username=?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, new_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    printf("  [DB] password_hash updated  user='%s'\n", username);
}

void update_totp_secret(const char *username, const char *secret)
{
    sqlite3 *db = _open_db();
    if (!db) return;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE users SET totp_secret=? WHERE username=?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, secret,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void delete_user(const char *username)
{
    sqlite3 *db = _open_db();
    if (!db) return;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "DELETE FROM users WHERE username=?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        printf("  [DB] deleted user='%s'\n", username);
    else
        fprintf(stderr, "  [DB] delete failed for user='%s': %s\n",
                username, sqlite3_errmsg(db));

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}
