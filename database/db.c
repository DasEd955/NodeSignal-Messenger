/* db.c -- SQLite persistence layer for NodeSignal Messenger.

Manages the database connection lifecycle, schema initialization, and all
DML operations (user upsert, message insert, message history retrieval).

Prepared statements are compiled once in ns_db_open and reused for the
lifetime of the connection; callers should not re-open the database between
operations.  All timestamps are stored as 64-bit integers to avoid the
Year-2038 truncation that would result from INT/bind_int.  The 32-bit
wire-protocol timestamp field is a separate, documented limitation.
*/

#include "db.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Canonical DDL for the NodeSignal schema.
   schema.sql in the repository root is provided for reference only;
   this string is the authoritative source.  Keep them in sync manually
   if the schema changes. */
static const char *NS_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS users ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    username TEXT UNIQUE NOT NULL,"
    "    created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS messages ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    sender_id INTEGER NOT NULL REFERENCES users(id),"
    "    body TEXT NOT NULL,"
    "    sent_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_sender_id ON messages (sender_id);";

/* ns_db_find_user_id -- Look up a user's integer ID by username.

Resets and rebinds the pre-compiled stmt_find_user statement rather than
preparing a new one, keeping overhead per call minimal.

Args:
    database:    Open database handle with stmt_find_user compiled.
    username:    Null-terminated username to search for.
    out_user_id: Receives the found user ID on success.

Returns:
    0 if the user exists and out_user_id is populated, -1 otherwise.
*/
static int ns_db_find_user_id(NsDatabase *database, const char *username, uint32_t *out_user_id) {
    sqlite3_stmt *statement = database->stmt_find_user;
    int step_result = 0;

    if(statement == NULL) {
        return -1;
    }

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_text(statement, 1, username, -1, SQLITE_TRANSIENT);
    step_result = sqlite3_step(statement);
    if(step_result == SQLITE_ROW) {
        *out_user_id = (uint32_t) sqlite3_column_int64(statement, 0);
        return 0;
    }

    return -1;
}

/* ns_db_open -- Open the SQLite database and compile all prepared statements.

Applies recommended PRAGMAs (foreign_keys, WAL journal mode, busy_timeout)
immediately after opening.  Compiles the four statements used by the server
so each DML operation can reset and rebind rather than prepare from scratch.

Args:
    database: Zeroed NsDatabase structure to populate.
    path:     Filesystem path to the SQLite database file.

Returns:
    0 on success, -1 on any failure (database is safe to call ns_db_close on).
*/
int ns_db_open(NsDatabase *database, const char *path) {
    if(database == NULL || path == NULL) {
        return -1;
    }

    memset(database, 0, sizeof(*database));
    if(sqlite3_open(path, &database->handle) != SQLITE_OK) {
        ns_db_close(database);
        return -1;
    }

    sqlite3_exec(database->handle, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    sqlite3_exec(database->handle, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(database->handle, 5000);

    /* Apply schema DDL before preparing statements so that statement compilation
       succeeds even on a brand-new or in-memory database. IF NOT EXISTS makes
       this idempotent when called against an existing on-disk database. */
    if(sqlite3_exec(database->handle, NS_SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        ns_db_close(database);
        return -1;
    }

    if(sqlite3_prepare_v2(database->handle,
            "SELECT id FROM users WHERE username = ?1;",
            -1, &database->stmt_find_user, NULL) != SQLITE_OK) {
        ns_db_close(database);
        return -1;
    }

    if(sqlite3_prepare_v2(database->handle,
            "INSERT OR IGNORE INTO users (username, created_at) VALUES (?1, ?2);",
            -1, &database->stmt_insert_user, NULL) != SQLITE_OK) {
        ns_db_close(database);
        return -1;
    }

    if(sqlite3_prepare_v2(database->handle,
            "INSERT INTO messages (sender_id, body, sent_at) VALUES (?1, ?2, ?3);",
            -1, &database->stmt_insert_message, NULL) != SQLITE_OK) {
        ns_db_close(database);
        return -1;
    }

    if(sqlite3_prepare_v2(database->handle,
            "SELECT u.username, m.body, m.sent_at"
            " FROM messages m JOIN users u ON u.id = m.sender_id"
            " ORDER BY m.id DESC LIMIT ?1;",
            -1, &database->stmt_recent_messages, NULL) != SQLITE_OK) {
        ns_db_close(database);
        return -1;
    }

    return 0;
}

/* ns_db_close -- Finalize all prepared statements and close the database connection.

Safe to call on a partially initialized database (e.g., after a failed
ns_db_open) because each statement pointer is checked before finalization.

Args:
    database: Database handle to tear down; ignored if NULL or already closed.
*/
void ns_db_close(NsDatabase *database) {
    if(database == NULL || database->handle == NULL) {
        return;
    }

    if(database->stmt_find_user != NULL) {
        sqlite3_finalize(database->stmt_find_user);
        database->stmt_find_user = NULL;
    }
    if(database->stmt_insert_user != NULL) {
        sqlite3_finalize(database->stmt_insert_user);
        database->stmt_insert_user = NULL;
    }
    if(database->stmt_insert_message != NULL) {
        sqlite3_finalize(database->stmt_insert_message);
        database->stmt_insert_message = NULL;
    }
    if(database->stmt_recent_messages != NULL) {
        sqlite3_finalize(database->stmt_recent_messages);
        database->stmt_recent_messages = NULL;
    }

    sqlite3_close(database->handle);
    database->handle = NULL;
}

/* ns_db_init_schema -- Execute the DDL to create tables and indexes if absent.

Idempotent: uses CREATE TABLE IF NOT EXISTS so re-running on an existing
database is safe.

Args:
    database: Open database handle.

Returns:
    0 on success, -1 on SQL error (message printed to stderr).
*/
int ns_db_init_schema(NsDatabase *database) {
    char *error_message = NULL;
    int rc = 0;

    if(database == NULL || database->handle == NULL) {
        return -1;
    }

    rc = sqlite3_exec(database->handle, NS_SCHEMA_SQL, NULL, NULL, &error_message);
    if(rc != SQLITE_OK) {
        if(error_message != NULL) {
            fprintf(stderr, "SQLite schema error: %s\n", error_message);
            sqlite3_free(error_message);
        }
        return -1;
    }

    return 0;
}

/* ns_db_get_or_create_user -- Atomically ensure a user row exists and return its ID.

Wraps the INSERT OR IGNORE + SELECT in a BEGIN IMMEDIATE transaction to prevent
a race between two concurrent JOIN requests for the same new username (the
single-threaded server makes this race hypothetical today, but the correct
pattern is documented here for future multi-threaded use).

Args:
    database:    Open database handle.
    username:    Username to upsert.
    out_user_id: Receives the user's integer ID on success.

Returns:
    0 on success with out_user_id populated, -1 on any failure.
*/
int ns_db_get_or_create_user(NsDatabase *database, const char *username, uint32_t *out_user_id) {
    sqlite3_stmt *statement = NULL;
    int rc = 0;
    int step_result = 0;
    char *errmsg = NULL;

    if(database == NULL || database->handle == NULL || username == NULL || out_user_id == NULL) {
        return -1;
    }

    statement = database->stmt_insert_user;
    if(statement == NULL) {
        return -1;
    }

    rc = sqlite3_exec(database->handle, "BEGIN IMMEDIATE;", NULL, NULL, &errmsg);
    if(rc != SQLITE_OK) {
        sqlite3_free(errmsg);
        return -1;
    }

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_text(statement, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64) time(NULL));

    step_result = sqlite3_step(statement);
    if(step_result != SQLITE_DONE) {
        sqlite3_exec(database->handle, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }

    rc = sqlite3_exec(database->handle, "COMMIT;", NULL, NULL, &errmsg);
    if(rc != SQLITE_OK) {
        sqlite3_free(errmsg);
        sqlite3_exec(database->handle, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }

    return ns_db_find_user_id(database, username, out_user_id);
}

/* ns_db_insert_message -- Persist a single chat message linked to its sender.

Uses a pre-compiled statement; timestamps are bound as 64-bit integers to
avoid the Year-2038 truncation of bind_int.

Args:
    database:  Open database handle.
    sender_id: Database user ID of the message author.
    body:      Null-terminated message text.
    timestamp: Unix timestamp (seconds) when the message was sent.

Returns:
    0 on success, -1 on failure.
*/
int ns_db_insert_message(NsDatabase *database, uint32_t sender_id, const char *body, uint32_t timestamp) {
    sqlite3_stmt *statement = NULL;
    int step_result = 0;

    if(database == NULL || database->handle == NULL || body == NULL) {
        return -1;
    }

    statement = database->stmt_insert_message;
    if(statement == NULL) {
        return -1;
    }

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_int64(statement, 1, (sqlite3_int64) sender_id);
    sqlite3_bind_text(statement, 2, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64) timestamp);

    step_result = sqlite3_step(statement);
    return step_result == SQLITE_DONE ? 0 : -1;
}

/* ns_db_last_error -- Return the human-readable SQLite error string for the last operation.

Args:
    database: Database handle to query; may be NULL.

Returns:
    A pointer owned by SQLite (valid until the next database call on this
    handle), or "Database Unavailable" if the handle is NULL.
*/
const char *ns_db_last_error(const NsDatabase *database) {
    if(database == NULL || database->handle == NULL) {
        return "Database Unavailable";
    }

    return sqlite3_errmsg(database->handle);
}

/* ns_db_recent_messages -- Fetch the N most recent messages and deliver them oldest-first.

Executes a pre-compiled SELECT (messages JOIN users ORDER BY id DESC LIMIT N),
buffers the rows internally, then invokes callback in ascending chronological
order so the caller always receives a conversation-order stream regardless of
how the query returns rows.

Args:
    database: Open database handle.
    limit:    Maximum rows to return; clamped to [1, 500].
    callback: Called once per row with (username, body, sent_at, userdata).
    userdata: Passed unchanged to every callback invocation.

Returns:
    0 on success, -1 on invalid arguments or statement failure.
*/
int ns_db_recent_messages(NsDatabase *database, int limit,
                           NsMessageCallback callback, void *userdata) {
    sqlite3_stmt *statement = NULL;

    struct NsHistoryRow {
        char username[64];
        char body[513];
        int64_t sent_at;
    };
    struct NsHistoryRow rows[500];
    int count = 0;
    int i = 0;

    if(database == NULL || database->handle == NULL || callback == NULL) {
        return -1;
    }

    if(limit < 1) {
        limit = 1;
    }
    if(limit > 500) {
        limit = 500;
    }

    statement = database->stmt_recent_messages;
    if(statement == NULL) {
        return -1;
    }

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_int(statement, 1, limit);

    while(sqlite3_step(statement) == SQLITE_ROW && count < limit) {
        const char *uname = (const char *) sqlite3_column_text(statement, 0);
        const char *body  = (const char *) sqlite3_column_text(statement, 1);
        int64_t sent_at   = (int64_t) sqlite3_column_int64(statement, 2);

        if(uname == NULL || body == NULL) {
            continue;
        }

        snprintf(rows[count].username, sizeof(rows[count].username), "%s", uname);
        snprintf(rows[count].body,     sizeof(rows[count].body),     "%s", body);
        rows[count].sent_at = sent_at;
        ++count;
    }

    /* Deliver oldest-first by reversing the DESC result set. */
    for(i = count - 1; i >= 0; --i) {
        callback(rows[i].username, rows[i].body, rows[i].sent_at, userdata);
    }

    return 0;
}
