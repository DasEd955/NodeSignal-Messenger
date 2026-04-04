/* ===================================================================================
server.h -- Defines the Public Interface of the Server Component
=================================================================================== */

#ifndef NS_SERVER_H
#define NS_SERVER_H

/* int ns_server_run -- Acts as entry point other code can call to start the server

    -- const char *port: The server port string; e.g. "5555"
    -- const char *database_path; The path to the database; e.g. "database/messages.db"
    */
int ns_server_run(const char *port, const char *database_path);

#endif
