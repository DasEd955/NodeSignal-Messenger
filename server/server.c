/* server.c - NodeSignal Messenger server implementation.

Accepts TCP clients, maintains per-client state, and routes protocol
packets to the appropriate handlers. Uses poll() instead of select() to
remove the FD_SETSIZE ceiling and support more than 1024 simultaneous
connections. All client sockets run in non-blocking mode; outbound data is
queued in per-client ring buffers and drained lazily on POLLOUT so no slow
receiver can stall a broadcast. A 1-second poll timeout drives the
auth-timeout sweep (NS_AUTH_TIMEOUT_SECS) and the graceful-shutdown check.
SIGINT/SIGTERM (POSIX) or the Windows console handler set
ns_server_shutdown_requested and the loop exits cleanly on the next wakeup.
*/

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

/* Maximum simultaneous clients. poll() has no inherent fd ceiling;
   this cap is a policy choice. */
#define NS_MAX_CLIENTS 1024

/* Seconds after accept() before an unjoined connection is reaped. */
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

enum {
    NS_HANDLE_OK = 0,
    NS_HANDLE_DISCONNECT = -1
};

/* Per-client outbound ring buffer. Bytes are appended by ns_client_enqueue_send()
   and drained when POLLOUT fires. One slot is kept empty so head == tail means empty. */
#define NS_SEND_BUF_SIZE (NS_PACKET_BODY_MAX * 4U)

typedef struct NsSendBuf {
    unsigned char data[NS_SEND_BUF_SIZE];
    size_t head; /* next byte to send */
    size_t tail; /* next byte to write */
} NsSendBuf;

typedef struct NsClient {
    ns_socket_t socket_fd;
    bool active;
    bool joined;
    uint32_t user_id;
    char username[NS_USERNAME_MAX + 1U];
    time_t connect_time; /* used to enforce NS_AUTH_TIMEOUT_SECS */
    NsSendBuf send_buf;
} NsClient;

/* poll_fds[0] always tracks the listen socket.
   poll_fds[1+i] mirrors clients[i].socket_fd. */
typedef struct NsServerState {
    ns_socket_t listen_socket;
    NsDatabase database;
    NsClient clients[NS_MAX_CLIENTS];
    struct pollfd poll_fds[1 + NS_MAX_CLIENTS];
    int client_count;
} NsServerState;

/* ns_client_enqueue_send - Append raw bytes to a client's outbound ring buffer.

    At each step:
        1. Computes available space; one slot is kept empty so head == tail means empty.
        2. If len exceeds available - 1, returns -1 (caller should disconnect the client).
        3. Copies data in one or two memcpy calls, wrapping around the ring if needed.
        4. Advances tail by len (modulo NS_SEND_BUF_SIZE if wrapped).

    Args:
        client: Target client whose send_buf receives the data.
        data:   Bytes to enqueue.
        len:    Number of bytes to enqueue.

    Returns:
        int: 0 on success, -1 if the buffer has insufficient space.
*/
static int ns_client_enqueue_send(NsClient *client, const unsigned char *data, size_t len) {
    size_t available = 0;
    size_t head = client->send_buf.head;
    size_t tail = client->send_buf.tail;

    if(len == 0) {
        return 0;
    }

    if(tail >= head) {
        available = NS_SEND_BUF_SIZE - (tail - head);
    } else {
        available = head - tail;
    }

    if(len > available - 1U) {
        return -1;
    }

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

/* ns_client_send_buf_pending - Return non-zero if there are bytes waiting to be sent. */
static int ns_client_send_buf_pending(const NsClient *client) {
    return client->send_buf.head != client->send_buf.tail;
}

/* ns_client_drain_send - Flush as many queued bytes as possible without blocking.

    Called when POLLOUT fires. Uses MSG_DONTWAIT (POSIX) or non-blocking sockets
    (Windows) so the call never stalls the event loop.

    At each step:
        1. Loops while the buffer has pending bytes.
        2. Computes a contiguous run of bytes from head to the end of the array or tail.
        3. Calls send() with no-wait flags.
        4. On EAGAIN/EWOULDBLOCK, stops and returns 0 (more data remains for next POLLOUT).
        5. On any other error, returns -1 so the caller can disconnect the client.
        6. Advances head by the number of bytes sent, wrapping with modulo.

    Args:
        client: Client whose outbound buffer should be flushed.

    Returns:
        int: 0 on success or EAGAIN, -1 if the socket is broken.
*/
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
            to_send = NS_SEND_BUF_SIZE - head;
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
                return 0;
            }
#else
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
#endif
            return -1;
        }

        client->send_buf.head = (head + (size_t)sent) % NS_SEND_BUF_SIZE;
    }

    return 0;
}

/* ns_server_update_poll_events - Sync a client's POLLOUT flag with its send buffer.

    Sets POLLIN always; adds POLLOUT only when the outbound ring buffer has
    pending bytes. Called after enqueuing data or completing a drain so the
    poll array always reflects the current outbound state.

    Args:
        server:       Server state containing the poll_fds array.
        client_index: Index of the client slot to update.
*/
static void ns_server_update_poll_events(NsServerState *server, int client_index) {
    short events = POLLIN;
    if(ns_client_send_buf_pending(&server->clients[client_index])) {
        events |= POLLOUT;
    }
    server->poll_fds[1 + client_index].events = events;
}

/* ns_server_enqueue_packet - Serialize a packet and enqueue it into one client's send buffer.

    Used for point-to-point sends such as ACK and ERROR.

    At each step:
        1. Validates server, client_index, and packet.
        2. Serializes the 14-byte wire header (version, type, sender_id, timestamp, body_len).
        3. Appends the body bytes if body_len > 0.
        4. Calls ns_client_enqueue_send() to copy the wire bytes into the ring buffer.
        5. Calls ns_server_update_poll_events() to arm POLLOUT if needed.

    Args:
        server:       Server state.
        client_index: Index of the target client in the clients array.
        packet:       Packet to serialize and queue.

    Returns:
        int: 0 on success, -1 on invalid arguments or buffer full.
*/
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

/* ns_server_reset_client - Return a client slot to its unoccupied default state. */
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

/* ns_server_init - Zero-initialize server state and set sentinel defaults.

    Args:
        server: Server state structure to initialize.
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

/* ns_server_find_free_slot - Return the index of the first inactive client slot.

    Returns:
        int: Slot index on success, -1 if all NS_MAX_CLIENTS slots are occupied.
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

/* ns_server_username_in_use - Return true if username is held by any active joined client.

    Args:
        server:     Server state to scan.
        username:   Null-terminated username to check.
        skip_index: Client index to exclude from the search (typically the
                    requesting client, to avoid a false self-collision).

    Returns:
        bool: true if another active joined client already owns the username.
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

/* ns_server_send_error - Enqueue an ERROR packet to client_index if that slot is active.

    Args:
        server:       Server state.
        client_index: Recipient client index.
        message:      Null-terminated error description to embed in the body.
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

/* Forward declaration required because ns_server_broadcast calls
   ns_server_disconnect_client which is defined after it. */
static void ns_server_disconnect_client(NsServerState *server, int client_index, bool announce_leave);

/* ns_server_broadcast - Serialize a packet once and enqueue it into every eligible client.

    Builds the wire bytes once, then copies them into each active joined
    client's outbound ring buffer (except exclude_index). A client whose buffer
    is full is disconnected rather than blocking the broadcast to others.

    At each step:
        1. Serializes the packet into a local wire buffer.
        2. Iterates all NS_MAX_CLIENTS slots; skips inactive, unjoined, or excluded.
        3. Calls ns_client_enqueue_send() for each eligible client.
        4. Disconnects any client whose buffer is full.
        5. Calls ns_server_update_poll_events() to arm POLLOUT for enqueued clients.

    Args:
        server:        Server state.
        packet:        Packet to broadcast.
        exclude_index: Client index to skip; pass -1 to include everyone.
*/
static void ns_server_broadcast(NsServerState *server, const NsPacket *packet, int exclude_index) {
    unsigned char wire[NS_PACKET_HEADER_SIZE + NS_PACKET_BODY_MAX];
    size_t wire_len = 0;
    int index = 0;

    if(packet == NULL || packet->header.body_len > NS_PACKET_BODY_MAX) {
        return;
    }

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
            ns_server_disconnect_client(server, index, false);
            continue;
        }
        ns_server_update_poll_events(server, index);
    }
}

/* ns_server_disconnect_client - Remove a client, optionally broadcasting a leave notice.

    At each step:
        1. Validates server and client_index; returns immediately if the slot is inactive.
        2. If announce_leave is true and the client joined, builds and broadcasts a
           "* username left the chat" LEAVE packet (excluding the departing client).
        3. Shuts down and closes the client socket.
        4. Removes the slot from the poll array and calls ns_server_reset_client().
        5. Decrements client_count, clamping to 0 to avoid negative values.

    Args:
        server:         Server state.
        client_index:   Slot to disconnect.
        announce_leave: If true and the client had joined, broadcasts a leave message.
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

    server->poll_fds[1 + client_index].fd = -1;
    server->poll_fds[1 + client_index].events = 0;
    server->poll_fds[1 + client_index].revents = 0;

    ns_server_reset_client(client);
    --server->client_count;
    if(server->client_count < 0) {
        server->client_count = 0;
    }
}

/* ns_server_join_validate - Check that a JOIN request is acceptable.

    At each step:
        1. Rejects the client if it is already joined.
        2. Rejects usernames with length outside [1, NS_USERNAME_MAX].
        3. Rejects usernames already held by another active joined client.
        4. Sends an ERROR packet for every rejection.

    Args:
        server:       Server state.
        client_index: Index of the client sending the JOIN packet.
        packet:       Received JOIN packet containing the requested username.

    Returns:
        bool: true if the join may proceed, false if an error packet was sent.
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

/* ns_server_join_finalize - Complete a validated JOIN by registering the user and notifying peers.

    At each step:
        1. Calls ns_db_get_or_create_user() to obtain a stable database ID.
        2. Marks the client as joined and stores the user_id and username.
        3. Builds and enqueues an ACK packet ("Connected as username") to the joiner.
        4. Builds and broadcasts a JOIN notice ("* username joined the chat") to everyone.
        5. Logs the join event to stdout.

    Args:
        server:       Server state.
        client_index: Index of the client completing the join.
        packet:       Received JOIN packet containing the username in its body.

    Returns:
        int: NS_HANDLE_OK on success, NS_HANDLE_DISCONNECT if any step fails.
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

/* ns_server_handle_join - Dispatch a JOIN packet through validation then finalization.

    Args:
        server:       Server state.
        client_index: Index of the client sending JOIN.
        packet:       Received JOIN packet.

    Returns:
        int: NS_HANDLE_OK on success, NS_HANDLE_DISCONNECT on validation or finalization failure.
*/
static int ns_server_handle_join(NsServerState *server, int client_index, const NsPacket *packet) {
    if(!ns_server_join_validate(server, client_index, packet)) {
        return NS_HANDLE_DISCONNECT;
    }

    return ns_server_join_finalize(server, client_index, packet);
}

/* ns_server_build_display_text - Format "username: message" into out.

    Args:
        client:   Client whose username prefixes the message.
        packet:   Packet whose body is the raw message text.
        out:      Destination buffer for the formatted string.
        out_size: Size of out in bytes.

    Returns:
        bool: true on success, false if the formatted string would overflow out_size.
*/
static bool ns_server_build_display_text(NsClient *client, const NsPacket *packet,
                                        char *out, size_t out_size) {
    int len = snprintf(out, out_size, "%s: %s", client->username, packet->body);
    return !(len < 0 || (size_t)len >= out_size);
}

/* ns_server_handle_text - Store and broadcast a TEXT message from a joined client.

    At each step:
        1. Rejects the packet if the client is not yet joined (disconnect).
        2. Rejects empty message bodies (keep connection).
        3. Builds "username: message" display text; rejects if it would overflow (keep connection).
        4. Calls ns_db_insert_message() to persist the raw body; disconnects on failure.
        5. Builds a TEXT broadcast packet from the display text.
        6. Calls ns_server_broadcast() to deliver to all connected clients.
        7. Logs the message to stdout.

    Args:
        server:       Server state.
        client_index: Index of the sending client.
        packet:       Received TEXT packet.

    Returns:
        int: NS_HANDLE_OK to keep the connection, NS_HANDLE_DISCONNECT to drop it.
*/
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

/* ns_server_set_nonblocking - Put socket_fd into non-blocking mode.

    Required so poll()-driven reads and writes never stall the event loop.

    Args:
        socket_fd: Socket to configure.
*/
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

/* ns_server_accept_client - Accept one incoming connection and assign it to a free slot.

    At each step:
        1. Calls accept() on the listen socket; returns immediately if no connection is ready.
        2. Puts the new socket into non-blocking mode.
        3. Finds a free client slot; if none exists, sends an ERROR and closes the socket.
        4. Resets the slot, stores the socket, marks it active, and records connect_time.
        5. Registers the socket in poll_fds with POLLIN and increments client_count.

    Args:
        server: Server state containing the listen socket and client array.
*/
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

    server->poll_fds[1 + slot_index].fd = (int) client_socket;
    server->poll_fds[1 + slot_index].events = POLLIN;
    server->poll_fds[1 + slot_index].revents = 0;
    ++server->client_count;

    printf("Accepted client on slot %d\n", slot_index);
}

/* ns_server_handle_client_readable - Read and dispatch one packet from a client whose POLLIN fired.

    EAGAIN/EWOULDBLOCK on a non-blocking socket means no complete packet is
    available yet; the client is left connected for the next poll wakeup.

    At each step:
        1. Calls ns_recv_packet(); disconnects on NS_RECV_CLOSED.
        2. On NS_RECV_ERROR, checks errno/WSAGetLastError; treats EAGAIN as "try again".
        3. Dispatches the packet type: JOIN -> handle_join, TEXT -> handle_text,
           LEAVE -> disconnect, default -> send error and disconnect.

    Args:
        server: Server state.
        index:  Index of the client slot to read from.
*/
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

/* ns_server_reap_auth_timeouts - Disconnect clients that accepted but never sent JOIN.

    Called once per poll() wakeup. Iterates all active, unjoined clients and
    disconnects those whose connect_time is older than NS_AUTH_TIMEOUT_SECS.

    Args:
        server: Server state.
*/
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

/* ns_server_run - Initialize and run the NodeSignal server event loop.

    At each step:
        1. Allocates NsServerState on the heap (it holds ~2 MB of send buffers).
        2. Installs SIGINT/SIGTERM handlers (POSIX) or a console handler (Windows).
        3. Opens and initializes the SQLite database.
        4. Binds the listen socket via ns_listen_tcp().
        5. Loops on poll() with a 1-second timeout:
            a. Checks ns_server_shutdown_requested and exits cleanly if set.
            b. Calls ns_server_reap_auth_timeouts() on every wakeup.
            c. Accepts new connections when poll_fds[0] is readable.
            d. Drains outbound buffers when POLLOUT fires for a client.
            e. Reads and dispatches packets when POLLIN fires for a client.
            f. Disconnects clients whose sockets report POLLERR/POLLHUP/POLLNVAL.
        6. Shuts down the listen socket, closes the database, and frees state.

    Args:
        port:          Port number string to listen on (e.g., "5555").
        database_path: Filesystem path to the SQLite database file.

    Returns:
        int: EXIT_SUCCESS on clean shutdown via signal, EXIT_FAILURE on error.
*/
int ns_server_run(const char *port, const char *database_path) {
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

        ns_server_reap_auth_timeouts(srv);

        if(poll_result == 0) {
            continue;
        }

        if(srv->poll_fds[0].revents & POLLIN) {
            ns_server_accept_client(srv);
        }

        for(index = 0; index < NS_MAX_CLIENTS; ++index) {
            NsClient *client = &srv->clients[index];
            struct pollfd *pfd = &srv->poll_fds[1 + index];

            if(!client->active || pfd->revents == 0) {
                continue;
            }

            if(pfd->revents & POLLOUT) {
                if(ns_client_drain_send(client) != 0) {
                    ns_server_disconnect_client(srv, index, client->joined);
                    continue;
                }
                ns_server_update_poll_events(srv, index);
            }

            if(pfd->revents & POLLIN) {
                ns_server_handle_client_readable(srv, index);
            }

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

/* main - Resolve database path and port, initialize networking, run the server.

    At each step:
        1. Calls ns_get_executable_dir() to build a default database path relative
           to the executable; falls back to "database/messages.db" on failure.
        2. Overrides port from argv[1] and database path from argv[2] if provided.
        3. Initializes the networking subsystem via ns_net_init().
        4. Calls ns_server_run() and stores its exit code.
        5. Cleans up networking with ns_net_cleanup() and returns the exit code.

    Args:
        argc: Number of command-line arguments.
        argv: Argument strings; argv[1] = port, argv[2] = database path (both optional).

    Returns:
        int: EXIT_SUCCESS or EXIT_FAILURE.
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
    } else {
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
