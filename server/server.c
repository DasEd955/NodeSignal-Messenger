/* ===================================================================================
server.c -- Defines the Logic for Running the Server 
    -- Implements the messenger server
    -- Accepts clients
    -- Handles packets
    -- Broadcasts messages
    -- Stores chat activity
=================================================================================== */

#include "server.h"
#include "comm.h"
#include "db.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Return codes utilized by server packet handler functions 
enum {
    NS_HANDLE_OK = 0,
    NS_HANDLE_DISCONNECT = -1
};

// typedef struct NsClient -- Represents a connected client on the server
    /*
    -- ns_socket_t socket_fd: The socket for the client's network connection
    -- bool active: Whether this client slot is currently in use
    -- bool joined: Whether the client has completed the chat join process
    -- uint32_t user_id: The numeric ID associated with the user from database
    -- char username[NS_USERNAME_MAX + 1U]: The client's username as a C string, including room for null terminator
    */
typedef struct NsClient {
    ns_socket_t socket_fd;
    bool active;
    bool joined;
    uint32_t user_id;
    char username[NS_USERNAME_MAX + 1U];
} NsClient;

// typedef struct NsServerState -- Represents the overall state of the server while running
    /*
    -- ns_socket_t listen_socket: The server's main listening socket; used to accept new client connections
    -- NsDatabase database: The database object the server uses to store & retrieve client data
    -- NsClient clients[FD_SETSIZE]: An array of client records. Each element stores information about one connected client
    */
typedef struct NsServerState {
    ns_socket_t listen_socket;
    NsDatabase database;
    NsClient clients[FD_SETSIZE];
} NsServerState;

// static void ns_server_reset_client -- Resets an NsClient record back to a clean default state
    /*
    -- Acts as a helper function for initializing or clearing a client slot
    -- Used by the server when a client slot is first prepared or after a client disconnects

    -- NsClient *client: The NsClient record being reset to default state

    -- If client is NULL, return immediately
    -- Otherwise, clear all of the client fields
        -- Sets socket_fd to NS_INVALID_SOCKET
        -- Sets active to false
        -- Sets joined to false
        -- Sets user_id to 0
        -- Clears the username array with memset()
    */
static void ns_server_reset_client(NsClient *client) {
    if (client == NULL) {return;}

    client->socket_fd = NS_INVALID_SOCKET;
    client->active = false;
    client->joined = false;
    client->user_id = 0U;
    memset(client->username, 0, sizeof(client->username));
}

// static void ns_server_init -- Initializes the server state to a clean default state
    /* 
    -- Acts as a helper function for preparing an NsServerState before the server starts running
    -- Used to clear all existing data in the server structure and set up default values
    
    -- NsServerState *server: The server state structure being initialized

    -- Assumes the server points to a valid NsServerState
    -- Declares size_t index = 0 to use as a loop counter
    -- Clears the entire server structure with memset()
    -- Sets listen_socket to NS_INVALID_SOCKET so the server starts with no valid listening socket
    -- Loops through all client slots from 0 to FD_SETSIZE - 1
        -- Calls ns_server_reset_client() on each client entry
        -- Ensures every client slot starts in an unused default state
    */
static void ns_server_init(NsServerState *server) {
    size_t index = 0;

    memset(server, 0, sizeof(*server));
    server->listen_socket = NS_INVALID_SOCKET;

    for(index = 0; index < FD_SETSIZE; ++index) {
        ns_server_reset_client(&server->clients[index]);
    }
}

// static int ns_server_find_free_slot -- Finds the index of the first unused client slot
    /*
    -- Acts as a helper function for locating an available position in the server's client array
    -- Used when a new client connects and the server needs a free slot to store that client's data

    -- const NsServerState *server: The server state containing the array of client records

    -- Assumes server points to a valid NsServerState
    -- Declares int index = 0 to use as a loop counter
    -- Loops through all client slots from 0 to FD_SETSIZE - 1
        -- Checks whether current client slot is inactive
        -- If the slot is inactive, returns that index immediately
    -- If no inactive client slot is found, returns -1
    -- A return value of -1 means the server has no free client slots available
    */
static int ns_server_find_free_slot(const NsServerState *server) {
    int index = 0;

    for(index = 0; index < FD_SETSIZE; ++index) {
        if(!server->clients[index].active) {
            return index;
        }
    }

    return -1;
}

// static bool ns_server_username_in_use -- Checks whether a username is already being used by another active joined client
    /*
    -- Acts as a helper function for validating usernames during the joining process
    -- Used to prevent multiple connected clients from using the same username simultaneously

    -- const NsServerState *server: The server state containing the array of client records
    -- const char *username: The username being checked for duplicate usage
    -- int skip_index: A client index to ignore during the search

    -- Assumes server points to a valid NsServerState
    -- Assumes username points to a valid null terminated string
    -- Declares int index = 0 to use as a loop counter
    -- Loops through all client slots from 0 to FD_SETSIZE - 1
        -- Creates a pointer to the current client record
        -- Skips the current slot if:
            -- The client is not active
            -- The client has not joined yet
            -- The current index matches skip_index
    -- Compares the client's username to the given username using strcmp()
    -- If the usernames match, return true immediately
    */
static bool ns_server_username_in_use(const NsServerState *server, const char *username, int skip_index) {
    int index = 0;

    for(index = 0; index < FD_SETSIZE; ++index) {
        const NsClient *client = &server->clients[index];
        if(!client->active || !client->joined || index == skip_index) {
            continue;
        }
        if(strcmp(client->username, username) == 0) {
            return true;
        }
    }

    return false;
}

// static void ns_server_send_error -- Builds & sends an ERROR packet to a client
    /*
    -- Acts as a helper function for reporting invalid requests or server side problems to a client
    -- Used when the server needs to notify a client that an operation failed or the request was not allowed

    -- NsClient *client: The client that should receive the error message
    -- const char *message: The error text to place within the packet body

    -- Declares NsPacket packet to store the outgoing error packet
    -- If client is NULL or client is inactive, return immediately
    -- Calls ns_packet_set() to build an ERROR packet
        -- Uses NS_PACKET_ERROR as the packet type
        -- Uses 0U as the User ID
        -- Uses ns_unix_time_now() as the packet timestamp
        -- Uses message as the packet body
    -- If ns_packet_set() fails, return immediately
    -- Calls ns_send_packet() to send the error packet to the client's socket
    -- Casts the return value to (void) as the function does not use that result
    */
static void ns_server_send_error(NsClient *client, const char *message) {
    NsPacket packet;

    if(client == NULL || !client->active) {return;}

    if(ns_packet_set(&packet, NS_PACKET_ERROR, 0U, ns_unix_time_now(), message) != 0) {return;}

    (void)ns_send_packet(client->socket_fd, &packet);
}

// static void ns_server_disconnect_client -- Forward Declaration
    /*
    -- Forward declaration of ns_server_disconnect_client()
    -- This is required as server.c defines ns_server_broadcast() first, which calls this function before the compiler has seen its full implementation
    -- Without forward declaration, the compiler would reach the call before knowing what the function is
    */
static void ns_server_disconnect_client(NsServerState *server, int client_index, bool announce_leave);

// static void ns_server_broadcast -- Sends a packet to all active joined clients, except an optional excluded client 
    /*
    -- Acts as a helper function for delivering server messages to multiple connected clients
    -- Used when the server broadcasts chat events such as joins, leaves, or text messages

    -- NsServerState *server: The server state containing the client records
        -- Assumes server points to a valid NsServerState
    -- const NsPacket *packet: The packet to send to the selected clients
        -- Assumes packet points to a valid NsPacket
    -- int exclude_index: The client index to skip while broadcasting

    -- Declares int index = 0 to use as a loop counter
    -- Loops through all clients slots frm 0 to FD_SETSIZE - 1
        -- Creates a pointer to the current client record
        -- Skips the current slot if:
            -- The client is not active
            -- The client has not joined
            -- The current index matches exclude_index
        -- Calls ns_send_packet() to send the packet to the client's socket
        -- If sending fails, calls ns_server_disconnect_client() to remove the client
    -- Sends the packet only to valid active joined clients that are not excluded 
    */
static void ns_server_broadcast(NsServerState *server, const NsPacket *packet, int exclude_index) {
    int index = 0;

    for(index = 0; index < FD_SETSIZE; ++index) {
        NsClient *client = &server->clients[index];
        if(!client->active || !client->joined || index == exclude_index) {
            continue;
        }

        if(ns_send_packet(client->socket_fd, packet) != 0) {
            ns_server_disconnect_client(server, index, false);
        }
    }
}

// static void ns_server_disconnect_client -- Disconnects a client from the server & optionally announces that the client have left the chat
    /*
    -- Acts as a helper function for removing a client from the server
    -- Used when a client disconnects, leaves normally, or is dropped due to an error

    -- NsServerState *server: The server state that contains the client records
    -- int client_index: The index of the client slot to disconnect
    -- bool announce_leave: Determines whether the server should notify other clients that this client left

    -- If server is NULL || client is outside valid client array range || client at client_index is not active, return immediately
    -- If announce_leave is true & client has already joined:
        -- Creates a leave message packet
        -- Broadcasts the leave message to the other connected clients
    -- Shuts down the client's socket connection
    -- Closes the client's socket
    -- Resets the client slot so it can be reused
    */
static void ns_server_disconnect_client(NsServerState *server, int client_index, bool announce_leave) {
    NsClient *client = NULL;

    if(server == NULL || client_index < 0 || client_index >= FD_SETSIZE) {
        return;
    }

    client = &server->clients[client_index];
    if(!client->active) {
        return;
    }

    if(announce_leave && client->joined) {
        NsPacket leave_packet;
        char leave_text[NS_PACKET_BODY_MAX + 1U];

        snprintf(leave_text, sizeof(leave_text), "* %s left the chat", client->username);
        if(ns_packet_set(&leave_packet,
                         NS_PACKET_LEAVE,
                         client->user_id,
                         ns_unix_time_now(),
                         leave_text) == 0) {
            ns_server_broadcast(server, &leave_packet, client_index);
        }

        printf("Client left: %s\n", client->username);
    }

    ns_socket_shutdown(client->socket_fd);
    ns_socket_close(client->socket_fd);
    ns_server_reset_client(client);
}

// DEV NOTE: Consider Splitting into ns_server_join_request() & ns_server_finalize_join()
    // ns_server_join_request() -- Checks already joined, username length, and duplicate usernames
    // ns_server_finalize_join() -- Stores the client state, sends the ACK, broadcasts the join message

// static int ns_server_handle_join -- Processes a client's JOIN packet & completes the chat join flow
    /*
    -- Acts as a helper function for handling a client's request to join the chat
    -- Used when the server receives an NS_PACKET_JOIN packet from a connected client

    -- NsServerState *server: The server state containing the database and client records
    -- int client_index: The index of the client sending the join request
    -- const NsPacket *packet: The received packet containing the requested username

    -- Creates a pointer to the client record at client_index
    -- Declares packets for:
        -- The ACK response sent back to the joining client
        -- The JOIN message broadcast to other connected clients
    -- Declares text buffers for:
        -- The connection status message
        -- The join announcement message
    -- Declares uint32_t user_id = 0U to store the user's database ID

    -- If the client has already joined, sends an error & returns NS_HANDLE_DISCONNECT
    -- If the username length is invalid, sends an error & returns NS_HANDLE_DISCONNECT
    -- If the username is already active, sends an error & returns NS_HANDLE_DISCONNECT

    -- Calls ns_db_get_or_create_user() to look up or create the user in the database
    -- If the database operation fails:
        -- Prints an error to stderr, sends an error packet to the client, and returns NS_HANDLE_DISCONNECT
    
    -- Marks the client as joined
    -- Stores the user's database ID in client->user_id
    -- Copies the username from the packer into client->username

    -- Builds an ACK packet with a message like "Connected as <username>"
        -- If building the ACK packet fails, return NS_HANDLE_DISCONNECT
    -- Sends the ACK packet to the client
        -- If sending the ACK packet fails, return NS_HANDLE_DISCONNECT

    -- Builds a JOIN packet with a message like "* <username> has joined the chat"
    -- If building the join packet succeeds, broadcasts it to connected clients

    -- Prints a join message to the server console
    -- Returns NS_HANDLE_OK upon success
    */
static int ns_server_handle_join(NsServerState *server, int client_index, const NsPacket *packet) {
    NsClient *client = &server->clients[client_index];
    NsPacket ack_packet;
    NsPacket join_packet;
    char status_text[NS_PACKET_BODY_MAX + 1U];
    char join_text[NS_PACKET_BODY_MAX + 1U];
    uint32_t user_id = 0U;

    if (client->joined) {
        ns_server_send_error(client, "This connection already joined the chat.");
        return NS_HANDLE_DISCONNECT;
    }
    if (packet->header.body_len == 0 || packet->header.body_len > NS_USERNAME_MAX) {
        ns_server_send_error(client, "Usernames must be between 1 and 32 characters.");
        return NS_HANDLE_DISCONNECT;
    }
    if (ns_server_username_in_use(server, packet->body, client_index)) {
        ns_server_send_error(client, "That username is already active.");
        return NS_HANDLE_DISCONNECT;
    }

    if (ns_db_get_or_create_user(&server->database, packet->body, &user_id) != 0) {
        fprintf(stderr, "Failed to create user '%s': %s\n",
                packet->body,
                ns_db_last_error(&server->database));
        ns_server_send_error(client, "The server could not register that username.");
        return NS_HANDLE_DISCONNECT;
    }

    client->joined = true;
    client->user_id = user_id;
    snprintf(client->username, sizeof(client->username), "%s", packet->body);

    snprintf(status_text, sizeof(status_text), "Connected as %s", client->username);
    if (ns_packet_set(&ack_packet, NS_PACKET_ACK, user_id, ns_unix_time_now(), status_text) != 0) {
        return NS_HANDLE_DISCONNECT;
    }

    if (ns_send_packet(client->socket_fd, &ack_packet) != 0) {
        return NS_HANDLE_DISCONNECT;
    }

    snprintf(join_text, sizeof(join_text), "* %s joined the chat", client->username);
    if (ns_packet_set(&join_packet, NS_PACKET_JOIN, user_id, ns_unix_time_now(), join_text) == 0) {
        ns_server_broadcast(server, &join_packet, -1);
    }

    printf("Client joined: %s\n", client->username);
    return NS_HANDLE_OK;
}

// static int ns_server_handle_text -- Processes a client's TEXT packet
    /*
    -- Acts as a helper function for handling chat messages sent by a connected client
    -- Used when the server receives an NS_PACKET_TEXT packet

    -- NsServerState *server: The server state containing the database & client records
    -- int client_index: The index of the client sending the message
    -- const NsPacket *packet: The received packet containing the message text

    -- Creates a pointer to the client record at client_index
    -- Declares NsPacket broadcast_packet to store the outgoing text packet 
    -- Declares display_text to store the formatted message shown to clients
    -- Declares timestamp to store the current Unix time for the message
    -- Declares display_length = 0 to store the length returned by snprintf()

    -- If the client has not joined yet:
        -- Sends an an error packet to the client & returns NS_HANDLE_DISCONNECT
    -- If the incoming message body is empty:
        -- Sends an error packet to the client & returns NS_HANDLE_OK since the client should remain connected
    
    -- Builds display_text in the format "<username>: <message>"
    -- If snprintf() fails or the formatted message is too long:
        -- Sends an error packet to the client & returns NS_HANDLE_OK since the client should remain connected
    
    -- Calls ns_db_insert_message() to store the message in the database
    -- If the database insert fails:
        -- Prints an error to stderr, sends an error packet to the client, returns NS_HANDLE_DISCONNECT
    
    -- Calls ns_packet_set() to build the outgoing TEXT packet
    -- If packet creation fails:
        -- Sends an error packet to the client & returns NS_HANDLE_OK since the client should remain connected
    
    -- Broadcasts the message to connected clients
    -- Prints the message to the server console
    -- Returns NS_HANDLE_OK upon success
    */
static int ns_server_handle_text(NsServerState *server, int client_index, const NsPacket *packet) {
    NsClient *client = &server->clients[client_index];
    NsPacket broadcast_packet;
    char display_text[NS_PACKET_BODY_MAX + 1U];
    uint32_t timestamp = ns_unix_time_now();
    int display_length = 0;

    if (!client->joined) {
        ns_server_send_error(client, "Join the chat before sending messages.");
        return NS_HANDLE_DISCONNECT;
    }
    if (packet->header.body_len == 0) {
        ns_server_send_error(client, "Messages cannot be empty.");
        return NS_HANDLE_OK;
    }

    // Developer Note -- Potentially Optimize this block
    display_length = snprintf(display_text, sizeof(display_text), "%s: %s", client->username, packet->body);
    if (display_length < 0 || (size_t) display_length >= sizeof(display_text)) {
        ns_server_send_error(client, "That message is too long once the username is added.");
        return NS_HANDLE_OK;
    }

    if (ns_db_insert_message(&server->database, client->user_id, packet->body, timestamp) != 0) {
        fprintf(stderr, "Failed to store message for '%s': %s\n", client->username, ns_db_last_error(&server->database));
        ns_server_send_error(client, "The server could not store that message.");
        return NS_HANDLE_DISCONNECT;
    }

    if (ns_packet_set(&broadcast_packet, NS_PACKET_TEXT, client->user_id, timestamp, display_text) != 0) {
        ns_server_send_error(client, "That message is too long.");
        return NS_HANDLE_OK;
    }

    ns_server_broadcast(server, &broadcast_packet, -1);
    printf("Message from %s: %s\n", client->username, packet->body);
    return NS_HANDLE_OK;
}

//
    /* 
    
    */
static void ns_server_accept_client(NsServerState *server) {
    struct sockaddr_storage address;
    ns_socklen_t address_length = (ns_socklen_t) sizeof(address);
    ns_socket_t client_socket = NS_INVALID_SOCKET;
    int slot_index = 0;

    client_socket = accept(server->listen_socket, (struct sockaddr *) &address, &address_length);
    if (!ns_socket_is_valid(client_socket)) {
        return;
    }

    slot_index = ns_server_find_free_slot(server);
    if (slot_index < 0) {
        NsPacket error_packet;
        ns_packet_set(&error_packet,
                      NS_PACKET_ERROR,
                      0U,
                      ns_unix_time_now(),
                      "The server is full right now.");
        (void) ns_send_packet(client_socket, &error_packet);
        ns_socket_shutdown(client_socket);
        ns_socket_close(client_socket);
        return;
    }

    ns_server_reset_client(&server->clients[slot_index]);
    server->clients[slot_index].socket_fd = client_socket;
    server->clients[slot_index].active = true;
    printf("Accepted client on slot %d\n", slot_index);
}

int ns_server_run(const char *port, const char *database_path) {
    NsServerState server;
    char error_buffer[256];

    ns_server_init(&server);

    if (ns_db_open(&server.database, database_path) != 0) {
        fprintf(stderr, "Unable to open database '%s'.\n", database_path);
        return EXIT_FAILURE;
    }

    if (ns_db_init_schema(&server.database) != 0) {
        fprintf(stderr, "Unable to initialize database schema.\n");
        ns_db_close(&server.database);
        return EXIT_FAILURE;
    }

    server.listen_socket = ns_listen_tcp(port, 16, error_buffer, sizeof(error_buffer));
    if (!ns_socket_is_valid(server.listen_socket)) {
        fprintf(stderr, "Unable to start server on port %s: %s\n", port, error_buffer);
        ns_db_close(&server.database);
        return EXIT_FAILURE;
    }

    printf("NodeSignal server listening on port %s\n", port);
    printf("Using database at %s\n", database_path);

    for (;;) {
        fd_set read_set;
        ns_socket_t max_socket = server.listen_socket;
        int select_result = 0;
        int index = 0;

        FD_ZERO(&read_set);
        FD_SET(server.listen_socket, &read_set);

        for (index = 0; index < FD_SETSIZE; ++index) {
            const NsClient *client = &server.clients[index];
            if (!client->active) {
                continue;
            }

            FD_SET(client->socket_fd, &read_set);
            if (client->socket_fd > max_socket) {
                max_socket = client->socket_fd;
            }
        }

        select_result = select((int) max_socket + 1, &read_set, NULL, NULL, NULL);
        if (select_result < 0) {
            ns_last_error_string(error_buffer, sizeof(error_buffer));
            fprintf(stderr, "select() failed: %s\n", error_buffer);
            break;
        }

        if (FD_ISSET(server.listen_socket, &read_set)) {
            ns_server_accept_client(&server);
        }

        for (index = 0; index < FD_SETSIZE; ++index) {
            NsClient *client = &server.clients[index];
            NsPacket packet;
            int receive_result = 0;

            if (!client->active || !FD_ISSET(client->socket_fd, &read_set)) {
                continue;
            }

            receive_result = ns_recv_packet(client->socket_fd, &packet);
            if (receive_result <= 0) {
                ns_server_disconnect_client(&server, index, client->joined);
                continue;
            }

            switch (packet.header.type) {
                case NS_PACKET_JOIN:
                    if (ns_server_handle_join(&server, index, &packet) != 0) {
                        ns_server_disconnect_client(&server, index, false);
                    }
                    break;
                case NS_PACKET_TEXT:
                    if (ns_server_handle_text(&server, index, &packet) != 0) {
                        ns_server_disconnect_client(&server, index, false);
                    }
                    break;
                case NS_PACKET_LEAVE:
                    ns_server_disconnect_client(&server, index, true);
                    break;
                default:
                    ns_server_send_error(client, "Unsupported packet type.");
                    ns_server_disconnect_client(&server, index, false);
                    break;
            }
        }
    }

    ns_socket_shutdown(server.listen_socket);
    ns_socket_close(server.listen_socket);
    ns_db_close(&server.database);
    return EXIT_FAILURE;
}

int main(int argc, char **argv) {
    const char *port = "5555";
    const char *database_path = "database/messages.db";
    int net_status = 0;
    int run_status = 0;

    if (argc >= 2) {
        port = argv[1];
    }
    if (argc >= 3) {
        database_path = argv[2];
    }

    net_status = ns_net_init();
    if (net_status != 0) {
        fprintf(stderr, "Network initialization failed.\n");
        return EXIT_FAILURE;
    }

    run_status = ns_server_run(port, database_path);
    ns_net_cleanup();
    return run_status;
}
