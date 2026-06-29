/* ===================================================================================
db.h -- Declares the SQLite database interface for storing users & messages
=================================================================================== */

#ifndef NS_DB_H
#define NS_DB_H

#include <stdint.h>

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* typedef struct NsDatabase -- Represents the database state used by the SQLite module

    -- Acts as the main database structure for managing the SQLite connection
    -- Stores the active SQLite handle and pre-compiled prepared statements

    -- sqlite3 *handle: The SQLite handle used for database operations
    -- sqlite3_stmt *stmt_find_user: Prepared SELECT for user lookup by username
    -- sqlite3_stmt *stmt_insert_user: Prepared INSERT OR IGNORE for new users
    -- sqlite3_stmt *stmt_insert_message: Prepared INSERT for new messages
    -- sqlite3_stmt *stmt_recent_messages: Prepared SELECT for recent message history
    */
typedef struct NsDatabase {
    sqlite3 *handle;
    sqlite3_stmt *stmt_find_user;
    sqlite3_stmt *stmt_insert_user;
    sqlite3_stmt *stmt_insert_message;
    sqlite3_stmt *stmt_recent_messages;
} NsDatabase;

/* int ns_db_open -- Opens a SQLite database connection
    
    -- NsDatabase *database: The database structure that will store the opened SQLite handle
    -- const char *path: The path to the SQLite database file 
    */
int ns_db_open(NsDatabase *database, const char *path);

/* void ns_db_close -- Closes an open SQLite database connection

    -- NsDatabase *database: The database structure whose SQLite handle should be closed
    */
void ns_db_close(NsDatabase *database);

/* int ns_db_init_schema -- Initializes the database schema if it does not already exist

    -- NsDatabase *database: The database structure whose SQLite connection will be used to create the schema
    */
int ns_db_init_schema(NsDatabase *database);

/* int ns_db_get_or_create_user -- Looks up an existing user or creates a new user in the database

    -- NsDatabase *database: The database structure whose SQLite connection will be used for the user lookup or insertion
    -- const char *username: The username to search for or create
    -- uint32_t *out_user_id: Output parameter that stores the user's database ID
    */
int ns_db_get_or_create_user(NsDatabase *database, const char *username, uint32_t *out_user_id);

/* int ns_db_insert_message -- Inserts a new chat message into the database

    -- NsDatabase *database: The database structure whose SQLite connection will be used to insert the message
    -- uint32_t sender_id: The database ID of the user who sent the message
    -- const char *body: The text body of the message
    -- uint32_t timestamp: The Unix timestamp for when the message was sent
    */
int ns_db_insert_message(NsDatabase *database, uint32_t sender_id, const char *body, uint32_t timestamp);

/* const char *ns_db_last_error -- Returns the most recent SQLite error message for the database connection

    -- const NsDatabase *database: The database structure whose SQLite connection will be checked for the last error message
    */
const char *ns_db_last_error(const NsDatabase *database);

/* typedef void (*NsMessageCallback) -- Callback type for ns_db_recent_messages

    -- Called once per row with the sender's username, message body, and Unix timestamp
    -- userdata: caller-supplied context pointer passed through unchanged
    */
typedef void (*NsMessageCallback)(const char *username, const char *body,
                                  int64_t sent_at, void *userdata);

/* int ns_db_recent_messages -- Retrieves the N most recent messages joined with sender usernames

    -- NsDatabase *database: open database handle
    -- int limit: maximum number of rows to return (clamped to 1–500)
    -- NsMessageCallback callback: called once per row in ascending chronological order
    -- void *userdata: passed unchanged to each callback invocation
    -- Returns 0 on success or -1 on failure
    */
int ns_db_recent_messages(NsDatabase *database, int limit,
                           NsMessageCallback callback, void *userdata);

#ifdef __cplusplus
}
#endif

#endif
