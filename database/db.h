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
    -- Used to store the active SQLite database handle that other database functions operate on

    -- sqlite3 *handle: The SQLite handle used for database operations
    */
typedef struct NsDatabase {
    sqlite3 *handle;
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

#ifdef __cplusplus
}
#endif

#endif
