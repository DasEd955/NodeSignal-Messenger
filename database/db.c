#include "db.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

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
    ");";

static int ns_db_find_user_id(NsDatabase *database,
                              const char *username,
                              uint32_t *out_user_id) {
    sqlite3_stmt *statement = NULL;
    int step_result = 0;
    int rc = 0;

    rc = sqlite3_prepare_v2(database->handle,
                            "SELECT id FROM users WHERE username = ?1;",
                            -1,
                            &statement,
                            NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(statement, 1, username, -1, SQLITE_TRANSIENT);
    step_result = sqlite3_step(statement);
    if (step_result == SQLITE_ROW) {
        *out_user_id = (uint32_t) sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
        return 0;
    }

    sqlite3_finalize(statement);
    return -1;
}

int ns_db_open(NsDatabase *database, const char *path) {
    if (database == NULL || path == NULL) {
        return -1;
    }

    memset(database, 0, sizeof(*database));
    if (sqlite3_open(path, &database->handle) != SQLITE_OK) {
        ns_db_close(database);
        return -1;
    }

    return 0;
}

void ns_db_close(NsDatabase *database) {
    if (database == NULL || database->handle == NULL) {
        return;
    }

    sqlite3_close(database->handle);
    database->handle = NULL;
}

int ns_db_init_schema(NsDatabase *database) {
    char *error_message = NULL;
    int rc = 0;

    if (database == NULL || database->handle == NULL) {
        return -1;
    }

    rc = sqlite3_exec(database->handle, NS_SCHEMA_SQL, NULL, NULL, &error_message);
    if (rc != SQLITE_OK) {
        if (error_message != NULL) {
            fprintf(stderr, "SQLite schema error: %s\n", error_message);
            sqlite3_free(error_message);
        }
        return -1;
    }

    return 0;
}

int ns_db_get_or_create_user(NsDatabase *database,
                             const char *username,
                             uint32_t *out_user_id) {
    sqlite3_stmt *statement = NULL;
    int rc = 0;
    int step_result = 0;

    if (database == NULL || database->handle == NULL || username == NULL || out_user_id == NULL) {
        return -1;
    }

    if (ns_db_find_user_id(database, username, out_user_id) == 0) {
        return 0;
    }

    rc = sqlite3_prepare_v2(database->handle,
                            "INSERT INTO users (username, created_at) VALUES (?1, ?2);",
                            -1,
                            &statement,
                            NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(statement, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, (int) time(NULL));

    step_result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (step_result != SQLITE_DONE) {
        return -1;
    }

    *out_user_id = (uint32_t) sqlite3_last_insert_rowid(database->handle);
    return 0;
}

int ns_db_insert_message(NsDatabase *database,
                         uint32_t sender_id,
                         const char *body,
                         uint32_t timestamp) {
    sqlite3_stmt *statement = NULL;
    int rc = 0;
    int step_result = 0;

    if (database == NULL || database->handle == NULL || body == NULL) {
        return -1;
    }

    rc = sqlite3_prepare_v2(database->handle,
                            "INSERT INTO messages (sender_id, body, sent_at) VALUES (?1, ?2, ?3);",
                            -1,
                            &statement,
                            NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_int(statement, 1, (int) sender_id);
    sqlite3_bind_text(statement, 2, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, (int) timestamp);

    step_result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return step_result == SQLITE_DONE ? 0 : -1;
}

const char *ns_db_last_error(const NsDatabase *database) {
    if (database == NULL || database->handle == NULL) {
        return "database unavailable";
    }

    return sqlite3_errmsg(database->handle);
}
