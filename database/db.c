/* ===================================================================================
db.c -- Implements the SQLite database access logic
    -- Opens & closes the database connection
    -- Initializes the database schema 
    -- Looks up or creates users
    -- Stores chat messages
    -- Reports SQLite error messages
=================================================================================== */

#include "db.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* static const char *NS_SCHEMA_SQL -- Stores the SQL schema used to initialize the database

    -- Acts as a string constant containing the SQL statements needed to create the database tables
    -- Used by ns_db_init_schema() to create the users and messages tables if they do not already exist

    -- Creates the users tables
    -- Creates the messages table
    -- Defines the relationship from messages.sender_id to users.id
    */
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

/* static int ns_db_find_user_id -- Looks up a user's database ID by username

    -- Acts as a helper function for retrieving an existing user's ID from the database
    -- Used internally by ns_db_get_or_create_user() before attempting to insert a new user
    
    -- NsDatabase *database: The database structure whose SQLite connection will be used for to query
    -- const char *username: The username to search for in the users table
    -- uint32_t *out_user_id: Output parameter that stores the matching user ID if found

    -- Delcares sqlite3_stmt *statement = NULL to hold the prepared SQL statement
    -- Declares int step_result = 0 to store the result of sqlite3_step()
    -- Declares int rc = 0 to store SQLite return codes

    -- Calls sqlite3_prepare_v2() to prepare a SELECT query for the user ID
    -- If statement preparation fails, return -1

    -- Calls sqlite_bind_text() to bind the given username to the SQL query
    -- Calls sqlite3_step() to execute the query

    -- If sqlite3_step() returns SQLITE_ROW:
        -- Reads the user ID from column 0
        -- Stores the ID in out_user_id
        -- Finalizes the statement
        -- Return 0
    
    -- If no matching row is found:
        -- Finalizes the statement
        -- Return -1
    */  
static int ns_db_find_user_id(NsDatabase *database, const char *username, uint32_t *out_user_id) {
    sqlite3_stmt *statement = NULL;
    int step_result = 0;
    int rc = 0;

    rc = sqlite3_prepare_v2(database->handle, "SELECT id FROM users WHERE username = ?1;", -1, &statement, NULL);
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

/* int ns_db_open -- Opens a SQLite database connection & stores the handle in the database structure

    -- Acts as a public database function for opening a SQLite database file
    -- Used to initialize an NsDatabase structure before other database operations are performed

    -- NsDatabase *database: The database structure that will store the opened SQLite handle
    -- const char *path: The path to the SQLite database file

    -- If database or path is NULL, return -1

    -- Clears the database structure with memset()
    -- Calls sqlite3_open() to open the database file & store the handle in database->handle
    -- If sqlite3_open() fails:
        -- Calls ns_db_closer() to clean up any partially opened handle
        -- Return -1
    
    -- Return 0 upon success
    */
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

/* void ns_db_close -- Closes an open SQLite database connection & clears the stored handle

    -- Acts as a public database function for safely closing a SQLite database connection
    -- Used when the program is finished using the database or when cleanup is needed after an error
    
    -- NsDatabase *database: The database structure whose SQLite handle should be closed

    -- If database or database->handle is NULL, return immediately
    -- Calls sqlite3_close() to close the SQLite connection
    -- Sets database->handle to NULL so the structure no longer points to a closed connection
    */
void ns_db_close(NsDatabase *database) {
    if (database == NULL || database->handle == NULL) {
        return;
    }

    sqlite3_close(database->handle);
    database->handle = NULL;
}

/* int ns_db_init_schema -- Initializes the database schema using the SQL schema string 

    -- Acts as a public database function for creating the required database tables
    -- Used after opening the database connection to ensure the schema exists before other database operations
    
    -- NsDatabase *database: The database structure whos SQLite connection will be used to create the schema

    -- Declares char *error_message = NULL to store any SQLite error message returned by sqlite3_exec()
    -- Declares int rc = 0 to store the SQLite return value

    -- If database or database->handle is NULL, return -1

    -- Calls sqlite3_exec() to execute NS_SCHEMA_SQL on the open database connection
    -- If sqlite3_exec() does not return SQLITE_OK:
        -- Checks whether SQLite provided an error message
        -- If an error message exists:
            -- Prints the schema error to stderr & frees the SQLite error message with sqlite3_free()
        -- Return -1
    
    -- Return 0 upon success
    */
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
