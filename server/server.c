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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#else
#include <windows.h>
/* Windows provides WSAPoll() via winsock2.h (already included by comm.h).
   Map the POSIX names so the rest of the file is platform-neutral. */
#define poll(fds, nfds, timeout) WSAPoll(fds, nfds, timeout)
typedef ULONG nfds_t;
#endif

/* Maximum simultaneous clients (replaces FD_SETSIZE ceiling from select() era).
   poll() has no inherent fd ceiling; this cap is a policy choice. */
#define NS_MAX_CLIENTS 1024

/* Seconds after accept() before an unjoin'd connection is reaped (#9). */
#define NS_AUTH_TIMEOUT_SECS 10

static volatile int ns_server_shutdown_requested = 0;

#ifndef _WIN32
static void ns_server_signal_handler(int signum) {
    (void) signum;
    ns_server_shutdown_requested = 1;
}
#else
static BOOL WINAPI ns_server_console_handler(DWORD ctrl_type) {
    if(ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        ns_server_shutdown_requested = 1;
        return TRUE;
    }
    return FALSE;
}
#endif

/* Return codes utilized by server packet handler functions. */
enum {
    NS_HANDLE_OK = 0,
    NS_HANDLE_DISCONNECT = -1
};

/* Per-client outbound write buffer (#7).
   Bytes are appended by ns_client_enqueue_send() and drained by
   ns_client_drain_send() when POLLOUT fires. */
#define NS_SEND_BUF_SIZE (NS_PACKET_BODY_MAX * 4U)

typedef struct NsSendBuf {
    unsigned char data[NS_SEND_BUF_SIZE];
    size_t head; /* index of next byte to send */
    size_t tail; /* index of next byte to write */
} NsSendBuf;

/* typedef struct NsClient -- Represents a connected client on the server

    -- ns_socket_t socket_fd: The socket for the client's network connection
    -- bool active: Whether this client slot is currently in use
    -- bool joined: Whether the client has completed the chat join process
    -- uint32_t user_id: The numeric ID associated with the user from database
    -- char username[NS_USERNAME_MAX + 1U]: The client's username as a C string
    -- time_t connect_time: Wall-clock time this slot was last activated (#9)
    -- NsSendBuf send_buf: Per-client outbound byte queue (#7)
    */
typedef struct NsClient {
    ns_socket_t socket_fd;
    bool active;
    bool joined;
    uint32_t user_id;
    char username[NS_USERNAME_MAX + 1U];
    time_t connect_time;
    NsSendBuf send_buf;
} NsClient;

/* typedef struct NsServerState -- Represents the overall state of the server while running

    -- ns_socket_t listen_socket: The server's main listening socket
    -- NsDatabase database: The database object the server uses to store & retrieve client data
    -- NsClient clients[NS_MAX_CLIENTS]: Client records; indexed to match poll_fds[1+i]
    -- struct pollfd poll_fds[1 + NS_MAX_CLIENTS]: poll() fd array.
         poll_fds[0]   = listen_socket
         poll_fds[1+i] = clients[i].socket_fd
    -- int client_count: Number of currently active client slots (for O(n) compaction)
    */
typedef struct NsServerState {
    ns_socket_t listen_socket;
    NsDatabase database;
    NsClient clients[NS_MAX_CLIENTS];
    struct pollfd poll_fds[1 + NS_MAX_CLIENTS];
    int client_count;
} NsServerState;

/* ns_client_enqueue_send -- Appends raw bytes to a client's outbound queue.
   Returns 0 on success or -1 if the queue has insufficient space. */
static int ns_client_enqueue_send(NsClient *client, const unsigned char *data, size_t len) {
    size_t available = 0;
    size_t head = client->send_buf.head;
    size_t tail = client->send_buf.tail;

    if(len == 0) {
        return 0;
    }

    /* Ring buffer: one slot is always kept empty so head==tail means empty. */
    if(tail >= head) {
        available = NS_SEND_BUF_SIZE - (tail - head);
    } else {
        available = head - tail;
    }

    if(len > available - 1U) {
        return -1; /* No room. */
    }

    /* Simple append — if tail + len wraps, split into two copies. */
    if(tail + len <= NS_SEND_BUF_SIZE) {
        memcpy(client->send_buf.data + tail, data, len);
        client->send_buf.tail = tail + len;
    } else {
        size_t first = NS_SEND_BUF_SIZE - tail;
        memcpy(client->send_buf.data + tail, data, first);
        memcpy(client->send_buf.data, data + first, len - first);
        client->send_buf.tail = len - first;
    }

    return 0;
}

/* ns_client_send_buf_pending -- Returns nonzero if there are bytes waiting to be sent. */
static int ns_client_send_buf_pending(const NsClient *client) {
    return client->send_buf.head != client->send_buf.tail;
}

/* ns_client_drain_send -- Flushes as many queued bytes as possible without blocking.
   Returns 0 on success or -1 if the socket is broken (caller should disconnect). */
static int ns_client_drain_send(NsClient *client) {
    while(ns_client_send_buf_pending(client)) {
        size_t head = client->send_buf.head;
        size_t tail = client->send_buf.tail;
        size_t to_send = 0;
#ifdef _WIN32
        int sent = 0;
#else
        ssize_t sent = 0;
#endif

        if(tail > head) {
            to_send = tail - head;
        } else {
            to_send = NS_SEND_BUF_SIZE - head; /* Up to wrap. */
        }

#ifdef _WIN32
        sent = send(client->socket_fd,
                    (const char *)(client->send_buf.data + head),
                    (int)to_send, 0);
#else
        sent = send(client->socket_fd,
                    client->send_buf.data + head,
                    to_send, MSG_DONTWAIT);
#endif

        if(sent <= 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if(err == WSAEWOULDBLOCK) {
                return 0; /* Try again next POLLOUT. */
            }
#else
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; /* Try again next POLLOUT. */
            }
#endif
            return -1; /* Real error. */
        }

        client->send_buf.head = (head + (size_t)sent) % NS_SEND_BUF_SIZE;
    }

    return 0;
}

/* ns_server_update_poll_events -- Syncs a client's POLLOUT flag with its send buffer state. */
static void ns_server_update_poll_events(NsServerState *server, int client_index) {
    short events = POLLIN;
    if(ns_client_send_buf_pending(&server->clients[client_index])) {
        events |= POLLOUT;
    }
    server->poll_fds[1 + client_index].events = events;
}

/* ns_server_enqueue_packet -- Serializes a packet and enqueues it into a specific client's
   send buffer (for point-to-point sends such as ACK and ERROR).
   Returns 0 on success or -1 on failure (buffer full or bad packet). */
static int ns_server_enqueue_packet(NsServerState *server, int client_index, const NsPacket *packet) {
    unsigned char wire[NS_PACKET_HEADER_SIZE + NS_PACKET_BODY_MAX];
    size_t wire_len = 0;
    uint32_t v = 0;
    NsClient *client = NULL;

    if(server == NULL || client_index < 0 || client_index >= NS_MAX_CLIENTS) {
        return -1;
    }
    client = &server->clients[client_index];

    if(packet == NULL || packet->header.body_len > NS_PACKET_BODY_MAX) {
        return -1;
    }

    wire[0] = packet->header.version;
    wire[1] = packet->header.type;
    v = htonl(packet->header.sender_id); memcpy(wire + 2,  &v, 4);
    v = htonl(packet->header.timestamp); memcpy(wire + 6,  &v, 4);
    v = htonl(packet->header.body_len);  memcpy(wire + 10, &v, 4);
    wire_len = NS_PACKET_HEADER_SIZE;

    if(packet->header.body_len > 0) {
        memcpy(wire + wire_len, packet->body, packet->header.body_len);
        wire_len += packet->header.body_len;
    }

    if(ns_client_enqueue_send(client, wire, wire_len) != 0) {
        return -1;
    }
    ns_server_update_poll_events(server, client_index);
    return 0;
}

/* static void ns_server_reset_client -- Resets an NsClient record back to a clean default state
    
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
    if(client == NULL) {return;}

    client->socket_fd = NS_INVALID_SOCKET;
    client->active = false;
    client->joined = false;
    client->user_id = 0U;
    client->connect_time = 0;
    memset(client->username, 0, sizeof(client->username));
    memset(&client->send_buf, 0, sizeof(client->send_buf));
}

/* static void ns_server_init -- Initializes the server state to a clean default state
     
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
    server->client_count = 0;

    for(index = 0; index < NS_MAX_CLIENTS; ++index) {
        ns_server_reset_client(&server->clients[index]);
        server->poll_fds[1 + index].fd = -1;
        server->poll_fds[1 + index].events = 0;
        server->poll_fds[1 + index].revents = 0;
    }

    server->poll_fds[0].fd = -1;
    server->poll_fds[0].events = POLLIN;
    server->poll_fds[0].revents = 0;
}

/* static int ns_server_find_free_slot -- Finds the index of the first unused client slot
    
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

    for(index = 0; index < NS_MAX_CLIENTS; ++index) {
        if(!server->clients[index].active) {
            return index;
        }
    }

    return -1;
}

/* static bool ns_server_username_in_use -- Checks whether a username is already being used by another active joined client
    
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

    for(index = 0; index < NS_MAX_CLIENTS; ++index) {
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

/* static void ns_server_send_error -- Builds & sends an ERROR packet to a client
    
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
static void ns_server_send_error(NsServerState *server, int client_index, const char *message) {
    NsPacket packet;
    NsClient *client = NULL;

    if(server == NULL || client_index < 0 || client_index >= NS_MAX_CLIENTS) {return;}
    client = &server->clients[client_index];
    if(!client->active) {return;}

    if(ns_packet_set(&packet, NS_PACKET_ERROR, 0U, ns_unix_time_now(), message) != 0) {return;}

    (void)ns_server_enqueue_packet(server, client_index, &packet);
}

/* static void ns_server_disconnect_client -- Forward Declaration
    
    -- Forward declaration of ns_server_disconnect_client()
    -- This is required as server.c defines ns_server_broadcast() first, which calls this function before the compiler has seen its full implementation
    -- Without forward declaration, the compiler would reach the call before knowing what the function is
    */
static void ns_server_disconnect_client(NsServerState *server, int client_index, bool announce_leave);

/* static void ns_server_broadcast -- Sends a packet to all active joined clients, except an optional excluded client 
    
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
/* ns_server_broadcast -- Serializes packet into bytes and enqueues it into each active
   joined client's outbound buffer (except exclude_index).  Actual transmission happens
   lazily when POLLOUT fires for each client, so a slow receiver never blocks others. */
static void ns_server_broadcast(NsServerState *server, const NsPacket *packet, int exclude_index) {
    /* Build the wire representation once, then copy it into each client's queue. */
    unsigned char wire[NS_PACKET_HEADER_SIZE + NS_PACKET_BODY_MAX];
    size_t wire_len = 0;
    int index = 0;

    if(packet == NULL || packet->header.body_len > NS_PACKET_BODY_MAX) {
        return;
    }

    /* Serialize header into wire buffer. */
    wire[0] = packet->header.version;
    wire[1] = packet->header.type;
    {
        uint32_t v;
        v = htonl(packet->header.sender_id); memcpy(wire + 2,  &v, 4);
        v = htonl(packet->header.timestamp); memcpy(wire + 6,  &v, 4);
        v = htonl(packet->header.body_len);  memcpy(wire + 10, &v, 4);
    }
    wire_len = NS_PACKET_HEADER_SIZE;

    if(packet->header.body_len > 0) {
        memcpy(wire + wire_len, packet->body, packet->header.body_len);
        wire_len += packet->header.body_len;
    }

    for(index = 0; index < NS_MAX_CLIENTS; ++index) {
        NsClient *client = &server->clients[index];
        if(!client->active || !client->joined || index == exclude_index) {
            continue;
        }

        if(ns_client_enqueue_send(client, wire, wire_len) != 0) {
            /* Buffer full — disconnect this slow client rather than blocking everyone. */
            ns_server_disconnect_client(server, index, false);
            continue;
        }
        ns_server_update_poll_events(server, index);
    }
}

/* static void ns_server_disconnect_client -- Disconnects a client from the server & optionally announces that the client have left the chat
    
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

    if(server == NULL || client_index < 0 || client_index >= NS_MAX_CLIENTS) {
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

    /* Remove this fd from the poll array. */
    server->poll_fds[1 + client_index].fd = -1;
    server->poll_fds[1 + client_index].events = 0;
    server->poll_fds[1 + client_index].revents = 0;

    ns_server_reset_client(client);
    --server->client_count;
    if(server->client_count < 0) {
        server->client_count = 0;
    }
}

/* static bool ns_server_join_validate -- Validates whether a client may join the chat

    -- Acts as a helper function for checking JOIN request constraints before processing
    -- Used to ensure the client is not already joined, the username is valid, and not already in use

    -- NsServerState *server: The server state containing the client records
    -- NsClient *client: The client attempting to join
    -- const NsPacket *packet: The JOIN packet containing the requested username

    -- If the client has already joined:
        -- Sends an error packet & returns false
    -- If the username length is invalid:
        -- Sends an error packet & returns false
    -- If the username is already in use:
        -- Sends an error packet & returns false

    -- Returns true if all validation checks pass
    */
static bool ns_server_join_validate(NsServerState *server, int client_index, const NsPacket *packet) {
    NsClient *client = &server->clients[client_index];

    if(client->joined) {
        ns_server_send_error(server, client_index, "This connection already joined the chat.");
        return false;
    }
    if(packet->header.body_len == 0 || packet->header.body_len > NS_USERNAME_MAX) {
        ns_server_send_error(server, client_index, "Usernames must be between 1 and 32 characters.");
        return false;
    }
    if(ns_server_username_in_use(server, packet->body, client_index)) {
        ns_server_send_error(server, client_index, "That username is already active.");
        return false;
    }
    return true;
}

/* static int ns_server_join_finalize -- Completes the client join process after validation

    -- Acts as a helper function for finalizing a successful JOIN request
    -- Used to register the user, update client state, and notify other clients

    -- NsServerState *server: The server state containing the database and client records
    -- NsClient *client: The client completing the join process
    -- const NsPacket *packet: The JOIN packet containing the username

    -- Declares ACK and JOIN packets for response and broadcast
    -- Declares buffers for status and join messages
    -- Declares user_id to store the database user ID

    -- Calls ns_db_get_or_create_user() to register or fetch the user
    -- If the database operation fails:
        -- Prints an error, sends an error packet, and returns NS_HANDLE_DISCONNECT

    -- Marks the client as joined
    -- Stores the user ID and copies the username into the client record

    -- Builds an ACK packet confirming the connection
    -- Sends the ACK packet to the client
        -- If sending fails, returns NS_HANDLE_DISCONNECT

    -- Builds a JOIN broadcast message
    -- If successful, sends it to all connected clients

    -- Prints a join message to the server console
    -- Returns NS_HANDLE_OK upon success
    */
static int ns_server_join_finalize(NsServerState *server, int client_index, const NsPacket *packet) {
    NsClient *client = &server->clients[client_index];
    NsPacket ack_packet;
    NsPacket join_packet;
    char status_text[NS_PACKET_BODY_MAX + 1U];
    char join_text[NS_PACKET_BODY_MAX + 1U];
    uint32_t user_id = 0U;

    if(ns_db_get_or_create_user(&server->database, packet->body, &user_id) != 0) {
        fprintf(stderr, "Failed to create user '%s': %s\n", packet->body, ns_db_last_error(&server->database));
        ns_server_send_error(server, client_index, "The server could not register that username.");
        return NS_HANDLE_DISCONNECT;
    }

    client->joined = true;
    client->user_id = user_id;
    memcpy(client->username, packet->body, (size_t)packet->header.body_len);
    client->username[packet->header.body_len] = '\0';

    snprintf(status_text, sizeof(status_text), "Connected as %s", client->username);
    if(ns_packet_set(&ack_packet, NS_PACKET_ACK, user_id, ns_unix_time_now(), status_text) != 0) {
        return NS_HANDLE_DISCONNECT;
    }

    if(ns_server_enqueue_packet(server, client_index, &ack_packet) != 0) {
        return NS_HANDLE_DISCONNECT;
    }

    snprintf(join_text, sizeof(join_text), "* %s joined the chat", client->username);
    if(ns_packet_set(&join_packet, NS_PACKET_JOIN, user_id, ns_unix_time_now(), join_text) == 0) {
        ns_server_broadcast(server, &join_packet, -1);
    }

    printf("Client joined: %s\n", client->username);
    return NS_HANDLE_OK;
}

/* static int ns_server_handle_join -- Processes a client's JOIN packet by delegating validation & finalization steps
    
    -- Acts as the primary handler for JOIN packets received from clients
    -- Used when the server receives an NS_PACKET_JOIN packet and needs to complete the join workflow

    -- NsServerState *server: The server state containing client records and database access
    -- int client_index: The index of the client attempting to join
    -- const NsPacket *packet: The received packet containing the requested username

    -- Creates a pointer to the client record at client_index

    -- Calls ns_server_join_validate() to verify:
        -- The client has not already joined
        -- The username length is valid
        -- The username is not already in use
    -- If validation fails:
        -- Returns NS_HANDLE_DISCONNECT to indicate the client should be disconnected

    -- Calls ns_server_join_finalize() to:
        -- Register or retrieve the user from the database
        -- Store client state (joined, user_id, username)
        -- Send an ACK packet to the client
        -- Broadcast a JOIN message to other clients

    -- Returns the result of ns_server_join_finalize()
        -- NS_HANDLE_OK on success
        -- NS_HANDLE_DISCONNECT on failure
    */
static int ns_server_handle_join(NsServerState *server, int client_index, const NsPacket *packet) {
    if(!ns_server_join_validate(server, client_index, packet)) {
        return NS_HANDLE_DISCONNECT;
    }

    return ns_server_join_finalize(server, client_index, packet);
}

/* static int ns_server_handle_text -- Processes a client's TEXT packet
    
    -- Acts as a helper function for handling chat messages sent by a connected client
    -- Used when the server receives an NS_PACKET_TEXT packet

    -- NsServerState *server: The server state containing the database & client records
    -- int client_index: The index of the client sending the message
    -- const NsPacket *packet: The received packet containing the message text

    -- Creates a pointer to the client record at client_index
    -- Declares NsPacket broadcast_packet to store the outgoing text packet 
    -- Declares display_text to store the formatted message shown to clients
    -- Declares timestamp to store the current Unix time for the message
    -- Uses ns_server_build_display_text() to safely format the outgoing message

    -- If the client has not joined yet:
        -- Sends an an error packet to the client & returns NS_HANDLE_DISCONNECT
    -- If the incoming message body is empty:
        -- Sends an error packet to the client & returns NS_HANDLE_OK since the client should remain connected
    
    -- Calls ns_server_build_display_text() to format the message as "<username>: <message>"
-- If formatting fails:
    -- Sends an error packet to the client
    -- Returns NS_HANDLE_OK since the client should remain connected
    
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
static bool ns_server_build_display_text(NsClient *client, const NsPacket *packet,
                                        char *out, size_t out_size) {
    int len = snprintf(out, out_size, "%s: %s", client->username, packet->body);
    return !(len < 0 || (size_t)len >= out_size);
}

static int ns_server_handle_text(NsServerState *server, int client_index, const NsPacket *packet) {
    NsClient *client = &server->clients[client_index];
    NsPacket broadcast_packet;
    char display_text[NS_PACKET_BODY_MAX + 1U];
    uint32_t timestamp = ns_unix_time_now();

    if (!client->joined) {
        ns_server_send_error(server, client_index, "Join the chat before sending messages.");
        return NS_HANDLE_DISCONNECT;
    }

    if (packet->header.body_len == 0) {
        ns_server_send_error(server, client_index, "Messages cannot be empty.");
        return NS_HANDLE_OK;
    }

    if (!ns_server_build_display_text(client, packet, display_text, sizeof(display_text))) {
        ns_server_send_error(server, client_index, "That message is too long once the username is added.");
        return NS_HANDLE_OK;
    }

    if (ns_db_insert_message(&server->database, client->user_id, packet->body, timestamp) != 0) {
        fprintf(stderr, "Failed to store message for '%s': %s\n",
                client->username,
                ns_db_last_error(&server->database));
        ns_server_send_error(server, client_index, "The server could not store that message.");
        return NS_HANDLE_DISCONNECT;
    }

    if (ns_packet_set(&broadcast_packet, NS_PACKET_TEXT, client->user_id, timestamp, display_text) != 0) {
        ns_server_send_error(server, client_index, "That message is too long.");
        return NS_HANDLE_OK;
    }

    ns_server_broadcast(server, &broadcast_packet, -1);
    printf("Message from %s: %s\n", client->username, packet->body);

    return NS_HANDLE_OK;
}

/* static void ns_server_accept_client -- Accepts a new client connection & assigns it to a free client slot
     
    -- Acts as a helper function for accepting incoming client connections on the server's listening socket
    -- Used when select() indicates that the listening socket is ready to accept a new connection

    -- NsServerState *server: The server state containing the listening socket and client records
    
    -- Declares sockaddr_storage address to hold the connecting client's network address
    -- Declares address_length as the size of the address structure used by accept()
    -- Declares client_socket & initializes it to NS_INVALID_SOCKET
    -- Declares slot_index = 0 to store the index of an available client slot

    -- Calls accept() on the server's listening socket to accept a new client connection
        -- If the returned client socket is invalid, returns immediately

    -- Calls ns_server_find_free_slot() to locate an unused client slot
    -- If no free slot is available:
        -- Declares an ERROR packet
        -- Builds the ERROR packet with the message "The server is full right now."
        -- Sends the error packet to the connecting client
        -- Shuts down & closes the client socket
        -- Returns immediately
    
    -- Calls ns_server_reset_client() on the chosen client slot to ensure it starts clean
    -- Stores the accepted socket in the client slot
    -- Marks the client slot as active
    -- Prints the accepted slot number to the server console
    */
/* ns_server_set_nonblocking -- Puts a socket into non-blocking mode so poll()-driven
   reads and writes never stall the server loop. */
static void ns_server_set_nonblocking(ns_socket_t socket_fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket_fd, FIONBIO, &mode);
#else
    {
        int flags = fcntl(socket_fd, F_GETFL, 0);
        if(flags >= 0) {
            fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
#endif
}

static void ns_server_accept_client(NsServerState *server) {
    struct sockaddr_storage address;
    ns_socklen_t address_length = (ns_socklen_t) sizeof(address);
    ns_socket_t client_socket = NS_INVALID_SOCKET;
    int slot_index = 0;

    client_socket = accept(server->listen_socket, (struct sockaddr *) &address, &address_length);
    if(!ns_socket_is_valid(client_socket)) {
        return;
    }

    ns_server_set_nonblocking(client_socket);

    slot_index = ns_server_find_free_slot(server);
    if(slot_index < 0) {
        NsPacket error_packet;
        ns_packet_set(&error_packet, NS_PACKET_ERROR, 0U, ns_unix_time_now(), "The server is full right now.");
        (void) ns_send_packet(client_socket, &error_packet);
        ns_socket_shutdown(client_socket);
        ns_socket_close(client_socket);
        return;
    }

    ns_server_reset_client(&server->clients[slot_index]);
    server->clients[slot_index].socket_fd = client_socket;
    server->clients[slot_index].active = true;
    server->clients[slot_index].connect_time = time(NULL);

    /* Register in the poll array, initially watching only for incoming data. */
    server->poll_fds[1 + slot_index].fd = (int) client_socket;
    server->poll_fds[1 + slot_index].events = POLLIN;
    server->poll_fds[1 + slot_index].revents = 0;
    ++server->client_count;

    printf("Accepted client on slot %d\n", slot_index);
}

// DEV NOTE: Consider splitting into helpers to improve readability
    // Possibly: ns_server_build_read_set() && ns_server_process_ready_clients()

/* int ns_server_run -- Starts the NodeSignal Server & runs the main server loop
    
    -- Acts as the main entry point for running the NodeSignal server
    -- Used to initializer server state, open the database, start listening for clients, and process network events

    -- const char *port: The port number the server should listen on
    -- const char *database_path: The path to the SQLite database file

    -- Declares NSServerState server to store the server's runtime state
    -- Declares error_buffer to store readable socket error messages

    -- Calls ns_server_init() to initialize the server state
    
    -- Calls ns_db_open() to open the database at database_path
        -- If opening the database fails:
            -- Prints an error to stderr & returns EXIT_FAILURE
    
    -- Calls ns_db_init_schema() to ensure the database schema exists
        -- If schema initialization fails:
            -- Prints an error to stderr, closes the database & returns EXIT_FAILURE
    
    -- Prints startup messages showing the listening port & database path

    -- Enters an infinite server loop
        -- Declares read_set to track sockets ready for reading
        -- Declares max_socket to store the highest socket value for select()
        -- Declares select_result to store the return value from select()
        -- Declares index = 0 to use as a loop counter

        -- Clears read_set with FD_ZERO()
        -- Adds the listening socket to read_set with FD_SET()

        -- Loops through all client slots from 0 to FD_SETSIZE - 1
            -- Skips inactive clients
            -- Adds each active client socket to read_set
            -- Updates max_socket if needed
        
        -- Calls select() to wait until the listening socket or a client socket is ready
            -- If select() fails:
                -- Stores the socket error text in error_buffer, prints the error to stderr, breaks out of the server loop
                
        -- If the listening socket is ready:
            -- Calls ns_server_accept_client() to accept a new client connection

        -- Loops through all client slots again
            -- Skips inactive clients or clients whose sockets are not ready
            -- Calls ns_recv_packet() to receive the next packet from the client
            -- If receiving fails or the client is disconnectedL
                -- Calls ns_server_disconnect_client() & continues to the next client
            
            -- Checks packet.header.type to determine how to handle the packet
                -- NS_PACKET_JOIN:
                    -- Calls ns_server_handle_join()
                    -- If the handler returns a nonzero value, disconnects the client
                -- NS_PACKET_TEXT:
                    -- Calls ns_server_handle_text()
                    -- If the handler returns a nonzero value, disconnects the client
                -- NS_PACKET_LEAVE:
                    -- Disconnects the client & announces the leave event
                -- default:
                    -- Sends an error packet for an unsupported packet type; disconnects the client
    
    -- After leaving the infinite server loop:
        -- Shuts down the listening socket
        -- Closes the listening socket
        -- Closes the database
        -- Returns EXIT_FAILURE
    */

/* ns_server_handle_client_readable -- Reads and dispatches one packet from a client whose
   POLLIN fired.  Non-blocking reads mean ns_recv_packet may return NS_RECV_ERROR with
   EAGAIN/EWOULDBLOCK — treated as "no data yet" rather than disconnect. */
static void ns_server_handle_client_readable(NsServerState *server, int index) {
    NsClient *client = &server->clients[index];
    NsPacket packet;
    int receive_result = 0;

    receive_result = ns_recv_packet(client->socket_fd, &packet);
    if(receive_result == NS_RECV_CLOSED) {
        ns_server_disconnect_client(server, index, client->joined);
        return;
    }
    if(receive_result == NS_RECV_ERROR) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if(err == WSAEWOULDBLOCK) {
            return;
        }
#else
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
#endif
        ns_server_disconnect_client(server, index, client->joined);
        return;
    }

    switch(packet.header.type) {
        case NS_PACKET_JOIN:
            if(ns_server_handle_join(server, index, &packet) != NS_HANDLE_OK) {
                ns_server_disconnect_client(server, index, false);
            }
            break;

        case NS_PACKET_TEXT:
            if(ns_server_handle_text(server, index, &packet) != NS_HANDLE_OK) {
                ns_server_disconnect_client(server, index, false);
            }
            break;

        case NS_PACKET_LEAVE:
            ns_server_disconnect_client(server, index, true);
            break;

        default:
            ns_server_send_error(server, index, "Unsupported packet type.");
            ns_server_disconnect_client(server, index, false);
            break;
    }
}

/* ns_server_reap_auth_timeouts -- Disconnects clients that accepted() but never sent a
   JOIN within NS_AUTH_TIMEOUT_SECS.  Called once per poll() wakeup cycle (#9). */
static void ns_server_reap_auth_timeouts(NsServerState *server) {
    time_t now = time(NULL);
    int index = 0;

    for(index = 0; index < NS_MAX_CLIENTS; ++index) {
        NsClient *client = &server->clients[index];
        if(!client->active || client->joined) {
            continue;
        }
        if(now - client->connect_time >= NS_AUTH_TIMEOUT_SECS) {
            printf("Auth timeout: slot %d\n", index);
            ns_server_send_error(server, index, "Authentication timeout.");
            ns_server_disconnect_client(server, index, false);
        }
    }
}

/* int ns_server_run -- Starts the NodeSignal Server & runs the main event loop
    
    -- Acts as the main entry point for running the NodeSignal server
    -- Used to initialize server state, open the database, start listening for clients, and process network events

    -- const char *port: The port number the server should listen on
    -- const char *database_path: The path to the SQLite database file

    -- Declares NsServerState server to store the server's runtime state
    -- Declares error_buffer to store readable socket error messages

    -- Calls ns_server_init() to initialize the server state to default values

    -- Calls ns_db_open() to open the database at database_path
        -- If opening fails:
            -- Prints an error to stderr
            -- Returns EXIT_FAILURE

    -- Calls ns_db_init_schema() to ensure required database tables exist
        -- If initialization fails:
            -- Prints an error to stderr
            -- Closes the database
            -- Returns EXIT_FAILURE

    -- Calls ns_listen_tcp() to create and bind the listening socket
        -- Uses a backlog of 16 pending connections
        -- If socket creation fails:
            -- Prints an error message including the socket error text
            -- Closes the database
            -- Returns EXIT_FAILURE

    -- Prints startup messages showing the active port and database path

    -- Enters an infinite server loop
        -- Declares fd_set read_set to track sockets ready for reading
        -- Declares max_socket to store the highest socket descriptor
        -- Declares select_result to store the result of select()

        -- Calls ns_server_build_read_set() to:
            -- Initialize read_set
            -- Add the listening socket
            -- Add all active client sockets
            -- Determine the maximum socket value

        -- Calls select() to wait for activity on any socket
            -- If select() fails:
                -- Retrieves the error string
                -- Prints the error to stderr
                -- Breaks out of the server loop

        -- If the listening socket is ready:
            -- Calls ns_server_process_new_connection() to accept a new client

        -- Calls ns_server_process_clients() to:
            -- Iterate through all active clients
            -- Receive packets from ready sockets
            -- Dispatch packets to the appropriate handlers
            -- Disconnect clients when necessary

    -- After exiting the loop:
        -- Shuts down and closes the listening socket
        -- Closes the database

    -- Returns EXIT_FAILURE to indicate the server terminated due to an error
*/
int ns_server_run(const char *port, const char *database_path) {
    /* Heap-allocated: NsServerState holds per-client send buffers (~2 MB total). */
    NsServerState *srv = (NsServerState *) calloc(1, sizeof(NsServerState));
    char error_buffer[256];
    int exit_code = EXIT_FAILURE;

    if(srv == NULL) {
        fprintf(stderr, "Out of memory: cannot allocate server state.\n");
        return EXIT_FAILURE;
    }

#ifndef _WIN32
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = ns_server_signal_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
#else
    SetConsoleCtrlHandler(ns_server_console_handler, TRUE);
#endif

    ns_server_init(srv);

    if(ns_db_open(&srv->database, database_path) != 0) {
        fprintf(stderr, "Unable to open database '%s'.\n", database_path);
        free(srv);
        return EXIT_FAILURE;
    }

    if(ns_db_init_schema(&srv->database) != 0) {
        fprintf(stderr, "Unable to initialize database schema.\n");
        ns_db_close(&srv->database);
        free(srv);
        return EXIT_FAILURE;
    }

    srv->listen_socket = ns_listen_tcp(port, 16, error_buffer, sizeof(error_buffer));
    if(!ns_socket_is_valid(srv->listen_socket)) {
        fprintf(stderr, "Unable to start server on port %s: %s\n", port, error_buffer);
        ns_db_close(&srv->database);
        free(srv);
        return EXIT_FAILURE;
    }

    printf("NodeSignal Server listening on port %s\n", port);
    printf("Using database at %s\n", database_path);

    /* poll_fds[0] always tracks the listen socket. */
    srv->poll_fds[0].fd = (int) srv->listen_socket;
    srv->poll_fds[0].events = POLLIN;

    for(;;) {
        int poll_result = 0;
        int index = 0;
        nfds_t nfds = 1 + (nfds_t) NS_MAX_CLIENTS;

        if(ns_server_shutdown_requested) {
            printf("Shutdown requested. Stopping server.\n");
            exit_code = EXIT_SUCCESS;
            break;
        }

        /* 1-second timeout lets the shutdown flag be polled promptly and
           also drives the auth-timeout sweep once per second. */
        poll_result = poll(srv->poll_fds, nfds, 1000);
        if(poll_result < 0) {
            if(ns_server_shutdown_requested) {
                printf("Shutdown requested. Stopping server.\n");
                exit_code = EXIT_SUCCESS;
                break;
            }
            ns_last_error_string(error_buffer, sizeof(error_buffer));
            fprintf(stderr, "poll() failed: %s\n", error_buffer);
            break;
        }

        /* Reap half-open connections that never sent JOIN (#9). */
        ns_server_reap_auth_timeouts(srv);

        if(poll_result == 0) {
            continue;
        }

        /* Check for a new incoming connection. */
        if(srv->poll_fds[0].revents & POLLIN) {
            ns_server_accept_client(srv);
        }

        /* Service each client slot. */
        for(index = 0; index < NS_MAX_CLIENTS; ++index) {
            NsClient *client = &srv->clients[index];
            struct pollfd *pfd = &srv->poll_fds[1 + index];

            if(!client->active || pfd->revents == 0) {
                continue;
            }

            /* Drain any pending outbound data first (#7). */
            if(pfd->revents & POLLOUT) {
                if(ns_client_drain_send(client) != 0) {
                    ns_server_disconnect_client(srv, index, client->joined);
                    continue;
                }
                ns_server_update_poll_events(srv, index);
            }

            /* Handle incoming data (#5). */
            if(pfd->revents & POLLIN) {
                ns_server_handle_client_readable(srv, index);
            }

            /* Connection reset or hangup. */
            if(pfd->revents & (POLLERR | POLLHUP | POLLNVAL)) {
                ns_server_disconnect_client(srv, index, client->joined);
            }
        }
    }

    ns_socket_shutdown(srv->listen_socket);
    ns_socket_close(srv->listen_socket);
    ns_db_close(&srv->database);
    free(srv);

    return exit_code;
}

/* int main -- Entry point of the NodeSignal Server program
     
    -- Acts as the program's starting point
    -- Used to resolve the database path, read optional CLI arguments, initialize networking, run the server, and clean up before exiting
    
    -- int argc: The number of CLI arguments passed to the program
    -- char **argv: The array of CLI argument strings

    -- Declares port & initializes it to the default port "5555"
    -- Declares database_path pointer for the resolved database location
    -- Declares default_database_path buffer to store a constructed database path
    -- Declares executable_dir buffer to store the directory of the running executable
    -- Declares net_status to store the result of network initialization
    -- Declares run_status to store the return value from ns_server_run()

    -- Calls ns_get_executable_dir() to determine the directory of the running executable
        -- If successful:
            -- Constructs a default database path relative to the executable directory
            -- On Windows:
                -- Uses suffix "\\database\\messages.db"
            -- On Unix/Linux:
                -- Uses suffix "/database/messages.db"
            -- Calculates max_exec to ensure the combined path will not exceed buffer size
            -- Calls snprintf() with precision specifier %.*s to safely truncate executable_dir if needed
            -- Stores the resulting path in default_database_path
            -- Sets database_path to default_database_path
        -- If retrieval fails:
            -- Falls back to "database/messages.db"

    -- If argc >= 2:
        -- Uses argv[1] as the port number instead of the default port
    -- If argc >= 3:
        -- Uses argv[2] as the database path instead of the resolved/default path
    
    -- Calls ns_net_init() to initialize the networking layer
    -- If network initialization fails:
        -- Prints an error message to stderr
        -- Returns EXIT_FAILURE
    
    -- Calls ns_server_run() to start & run the server using the resolved port and database path
    -- Stores the return value in run_status

    -- Calls ns_net_cleanup() to release networking resources
    -- Returns run_status as the program's final exit code
*/
int main(int argc, char **argv) {
    const char *port = "5555";
    const char *database_path = NULL;
    char default_database_path[1024];
    char executable_dir[1024];
    int net_status = 0;
    int run_status = 0;

        if(ns_get_executable_dir(executable_dir, sizeof(executable_dir)) == 0) {
    #ifdef _WIN32
        {
            const char *suffix = "\\database\\messages.db";
            int max_exec = (int)(sizeof(default_database_path) - strlen(suffix) - 1);
            if(max_exec < 0) max_exec = 0;
            snprintf(default_database_path, sizeof(default_database_path), "%.*s%s", max_exec, executable_dir, suffix);
        }
    #else
        {
            const char *suffix = "/database/messages.db";
            int max_exec = (int)(sizeof(default_database_path) - strlen(suffix) - 1);
            if(max_exec < 0) max_exec = 0;
            snprintf(default_database_path, sizeof(default_database_path), "%.*s%s", max_exec, executable_dir, suffix);
        }
    #endif
        database_path = default_database_path;
        }
    else {
        database_path = "database/messages.db";
    }

    if(argc >= 2) {
        port = argv[1];
    }
    if(argc >= 3) {
        database_path = argv[2];
    }

    net_status = ns_net_init();
    if(net_status != 0) {
        fprintf(stderr, "Network initialization failed.\n");
        return EXIT_FAILURE;
    }

    run_status = ns_server_run(port, database_path);
    ns_net_cleanup();
    return run_status;
}
