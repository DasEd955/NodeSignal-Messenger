/* comm.c - Shared networking and packet protocol implementation.

Provides cross-platform socket helpers, TCP connect/listen factories, and
the binary wire protocol used between NodeSignal clients and servers. All
multi-byte header fields are transmitted in big-endian (network) byte order.
The in-memory NsPacketHeader struct is NOT layout-compatible with the wire
bytes; ns_send_packet and ns_recv_packet serialize every field individually.

Wire header layout (14 bytes, tightly packed):
  [0]      version   -- must equal NS_PROTOCOL_VERSION; receiver rejects mismatches
  [1]      type      -- NsPacketType enum value
  [2..5]   sender_id -- big-endian uint32
  [6..9]   timestamp -- big-endian uint32 Unix seconds
  [10..13] body_len  -- big-endian uint32, <= NS_PACKET_BODY_MAX
*/

#include "comm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#endif

/* ns_store_u32 - Write a uint32 into buf in network (big-endian) byte order.
   Used when serializing packet header fields before transmission. */
static void ns_store_u32(unsigned char *buffer, uint32_t value) {
    uint32_t network_value = htonl(value);
    memcpy(buffer, &network_value, sizeof(network_value));
}

/* ns_load_u32 - Read a big-endian uint32 from buf and return it in host order.
   Used when deserializing packet header fields after reception. */
static uint32_t ns_load_u32(const unsigned char *buffer) {
    uint32_t network_value = 0;
    memcpy(&network_value, buffer, sizeof(network_value));
    return ntohl(network_value);
}

/* ns_send_all - Reliably transmit exactly buffer_size bytes over socket_fd.

    Loops over send() until every byte is delivered, retrying on short writes.

    Args:
        socket_fd:   Connected socket to write to.
        buffer:      Bytes to transmit.
        buffer_size: Exact number of bytes to send.

    Returns:
        int: 0 on success, -1 if the socket reports an error or closes mid-send.
*/
static int ns_send_all(ns_socket_t socket_fd, const unsigned char *buffer, size_t buffer_size) {
    size_t total_sent = 0;

    while(total_sent < buffer_size) {
#ifdef _WIN32
        int sent_now = send(socket_fd, (const char *) buffer + total_sent, (int) (buffer_size - total_sent), 0);
#else
        ssize_t sent_now = send(socket_fd, buffer + total_sent, buffer_size - total_sent, 0);
#endif
        if(sent_now <= 0) {
            return -1;
        }
        total_sent += (size_t) sent_now;
    }

    return 0;
}

/* ns_recv_all - Reliably receive exactly buffer_size bytes from socket_fd.

    Loops over recv() until the full byte count arrives. Returns 0 (not -1)
    when the peer closes the connection before any bytes were read, to
    distinguish a clean EOF from a mid-message disconnect.

    Args:
        socket_fd:   Connected socket to read from.
        buffer:      Destination buffer; must be at least buffer_size bytes.
        buffer_size: Exact number of bytes to receive.

    Returns:
        int: 1 on success, 0 on clean EOF before the first byte, -1 on error
        or partial disconnect mid-message.
*/
static int ns_recv_all(ns_socket_t socket_fd, unsigned char *buffer, size_t buffer_size) {
    size_t total_received = 0;

    while(total_received < buffer_size) {
#ifdef _WIN32
        int received_now = recv(socket_fd, (char *) buffer + total_received, (int) (buffer_size - total_received), 0);
#else
        ssize_t received_now = recv(socket_fd, buffer + total_received, buffer_size - total_received, 0);
#endif
        if(received_now == 0) {
            return total_received == 0 ? 0 : -1;
        }
        if(received_now < 0) {
            return -1;
        }
        total_received += (size_t) received_now;
    }

    return 1;
}

/* ns_net_init - Initialize the OS networking subsystem.

    On Windows, calls WSAStartup to prepare Winsock before any socket
    operations. On POSIX, no setup is required.

    Returns:
        int: 0 on success, non-zero on failure (Windows WSAStartup error code).
*/
int ns_net_init(void) {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
#else
    return 0;
#endif
}

/* ns_net_cleanup - Release OS networking resources before program exit.

    On Windows, calls WSACleanup. On POSIX, no-op.
*/
void ns_net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

/* ns_socket_close - Close socket_fd if it is valid; otherwise do nothing. */
void ns_socket_close(ns_socket_t socket_fd) {
    if(!ns_socket_is_valid(socket_fd)) {
        return;
    }

#ifdef _WIN32
    closesocket(socket_fd);
#else
    close(socket_fd);
#endif
}

/* ns_socket_shutdown - Disable both send and receive directions on socket_fd.

    Signals the peer that no more data will be sent or received before the
    socket descriptor is released.

    Args:
        socket_fd: Socket to shut down.

    Returns:
        int: 0 if the socket was invalid (no-op), otherwise the result of shutdown().
*/
int ns_socket_shutdown(ns_socket_t socket_fd) {
    if(!ns_socket_is_valid(socket_fd)) {
        return 0;
    }

#ifdef _WIN32
    return shutdown(socket_fd, SD_BOTH);
#else
    return shutdown(socket_fd, SHUT_RDWR);
#endif
}

/* ns_socket_is_valid - Return non-zero if socket_fd is a usable socket handle. */
int ns_socket_is_valid(ns_socket_t socket_fd) {
    return socket_fd != NS_INVALID_SOCKET;
}

/* ns_unix_time_now - Return the current Unix timestamp as a uint32.

    The uint32 wire field is a known Year-2038 limitation, documented in the
    threat model. Returns 0 if the system clock reports an error.

    Returns:
        uint32_t: Seconds since the Unix epoch, truncated to 32 bits.
*/
uint32_t ns_unix_time_now(void) {
    time_t now = time(NULL);
    if(now < 0) {
        return 0U;
    }
    return (uint32_t) now;
}

/* ns_get_executable_dir - Write the directory containing the running executable into buffer.

    Used to locate assets and the database at a path relative to the binary,
    independent of the process working directory. On Windows uses
    GetModuleFileNameA; on Linux uses /proc/self/exe (Linux-only limitation
    documented in the threat model). Falls back to "." on failure.

    At each step:
        1. Returns -1 immediately if buffer is NULL or buffer_size is 0.
        2. Reads the executable path via platform-specific API.
        3. Scans backward for the last path separator and truncates there.
        4. Falls back to "." if no separator is found.

    Args:
        buffer:      Destination for the null-terminated directory path.
        buffer_size: Size of buffer in bytes.

    Returns:
        int: 0 on success, -1 on failure (buffer is set to an empty string on failure).
*/
int ns_get_executable_dir(char *buffer, size_t buffer_size) {
    size_t index = 0;

    if(buffer == NULL || buffer_size == 0) {
        return -1;
    }

#ifdef _WIN32
    {
        DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD) buffer_size);
        if(length == 0 || (size_t) length >= buffer_size) {
            buffer[0] = '\0';
            return -1;
        }
    }
#else
    {
        ssize_t length = readlink("/proc/self/exe", buffer, buffer_size - 1U);
        if(length < 0 || (size_t) length >= buffer_size) {
            buffer[0] = '\0';
            return -1;
        }
        buffer[length] = '\0';
    }
#endif

    for(index = strlen(buffer); index > 0; --index) {
        if(buffer[index - 1U] == '/' || buffer[index - 1U] == '\\') {
            buffer[index - 1U] = '\0';
            return 0;
        }
    }

    snprintf(buffer, buffer_size, ".");
    return 0;
}

/* ns_last_error_string - Retrieve the most recent socket or system error as text.

    On Windows, formats the WSAGetLastError code via FormatMessageA.
    On POSIX, uses strerror(errno).

    Args:
        buffer:      Destination for the null-terminated error string.
        buffer_size: Size of buffer in bytes.

    Returns:
        const char*: buffer, or an empty string literal if buffer is NULL or empty.
*/
const char *ns_last_error_string(char *buffer, size_t buffer_size) {
    if(buffer == NULL || buffer_size == 0) {
        return "";
    }

#ifdef _WIN32
    DWORD error_code = WSAGetLastError();
    DWORD copied = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error_code,
                                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, (DWORD) buffer_size, NULL);
    if(copied == 0) {
        snprintf(buffer, buffer_size, "winsock error %lu", (unsigned long) error_code);
    }
#else
    snprintf(buffer, buffer_size, "%s", strerror(errno));
#endif

    return buffer;
}

/* ns_try_connect_addrinfo - Attempt a TCP connection across each candidate address.

    At each step:
        1. Iterates the getaddrinfo result list.
        2. Creates a socket for each candidate; skips on failure.
        3. Calls connect(); returns the socket immediately on success.
        4. Closes the socket and resets it to NS_INVALID_SOCKET on connect failure.

    Args:
        results: Linked list of address candidates from getaddrinfo().

    Returns:
        ns_socket_t: A connected socket on success, NS_INVALID_SOCKET if all fail.
*/
static ns_socket_t ns_try_connect_addrinfo(struct addrinfo *results) {
    struct addrinfo *candidate = NULL;
    ns_socket_t socket_fd = NS_INVALID_SOCKET;

    for(candidate = results; candidate != NULL; candidate = candidate->ai_next) {
        socket_fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if(!ns_socket_is_valid(socket_fd)) {
            continue;
        }

        if(connect(socket_fd, candidate->ai_addr, (ns_socklen_t) candidate->ai_addrlen) == 0) {
            return socket_fd;
        }

        ns_socket_close(socket_fd);
        socket_fd = NS_INVALID_SOCKET;
    }

    return NS_INVALID_SOCKET;
}

/* ns_try_bind_listen - Bind and listen on the first viable address from the list.

    Enables SO_REUSEADDR on every candidate. IPv6 candidates additionally set
    IPV6_V6ONLY=0 so one socket accepts both IPv4 and IPv6 connections
    (dual-stack). This behavior applies on all platforms, not just Windows,
    which is intentional for portability.

    At each step:
        1. Iterates the getaddrinfo result list.
        2. Creates a socket; skips on failure.
        3. Sets SO_REUSEADDR; sets IPV6_V6ONLY=0 for IPv6 candidates.
        4. Calls bind(); closes and skips on failure.
        5. Calls listen(); returns the socket on success, closes and skips on failure.

    Args:
        results: Linked list of address candidates from getaddrinfo().
        backlog: Maximum number of queued pending connections for listen().

    Returns:
        ns_socket_t: A listening socket on success, NS_INVALID_SOCKET if all fail.
*/
static ns_socket_t ns_try_bind_listen(struct addrinfo *results, int backlog) {
    struct addrinfo *candidate = NULL;
    ns_socket_t listen_socket = NS_INVALID_SOCKET;
    int reuse = 1;

    for(candidate = results; candidate != NULL; candidate = candidate->ai_next) {
        int dual_stack = 0;

        listen_socket = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if(!ns_socket_is_valid(listen_socket)) {
            continue;
        }

        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, (ns_socklen_t) sizeof(reuse));

        if(candidate->ai_family == AF_INET6) {
            dual_stack = 0;
            setsockopt(listen_socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &dual_stack, (ns_socklen_t) sizeof(dual_stack));
        }

        if(bind(listen_socket, candidate->ai_addr, (ns_socklen_t) candidate->ai_addrlen) != 0) {
            ns_socket_close(listen_socket);
            listen_socket = NS_INVALID_SOCKET;
            continue;
        }

        if(listen(listen_socket, backlog) == 0) {
            return listen_socket;
        }

        ns_socket_close(listen_socket);
        listen_socket = NS_INVALID_SOCKET;
    }

    return NS_INVALID_SOCKET;
}

/* ns_connect_tcp - Resolve host:port and return a connected TCP socket.

    At each step:
        1. Configures getaddrinfo hints for AF_UNSPEC TCP.
        2. Calls getaddrinfo(); writes a readable error to error_buffer on failure.
        3. Calls ns_try_connect_addrinfo() to attempt connection across candidates.
        4. Writes ns_last_error_string() to error_buffer if no connection succeeds.
        5. Frees the getaddrinfo result list.

    Args:
        host:              Hostname or IP address of the remote server.
        port:              Port number as a decimal string (e.g., "5555").
        error_buffer:      If non-NULL, receives a human-readable error on failure.
        error_buffer_size: Size of error_buffer in bytes.

    Returns:
        ns_socket_t: A connected socket on success, NS_INVALID_SOCKET on failure.
*/
ns_socket_t ns_connect_tcp(const char *host, const char *port, char *error_buffer, size_t error_buffer_size) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    ns_socket_t socket_fd = NS_INVALID_SOCKET;
    int status = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    status = getaddrinfo(host, port, &hints, &results);
    if(status != 0) {
        if(error_buffer != NULL && error_buffer_size > 0) {
#ifdef _WIN32
            snprintf(error_buffer, error_buffer_size, "%s", gai_strerrorA(status));
#else
            snprintf(error_buffer, error_buffer_size, "%s", gai_strerror(status));
#endif
        }
        return NS_INVALID_SOCKET;
    }

    socket_fd = ns_try_connect_addrinfo(results);

    if(!ns_socket_is_valid(socket_fd) && error_buffer != NULL && error_buffer_size > 0) {
        ns_last_error_string(error_buffer, error_buffer_size);
    }

    freeaddrinfo(results);
    return socket_fd;
}

/* ns_listen_tcp - Bind to port and return a TCP listening socket.

    At each step:
        1. Configures getaddrinfo hints for AF_UNSPEC TCP with AI_PASSIVE.
        2. Calls getaddrinfo() with a NULL host to bind to all interfaces.
        3. Calls ns_try_bind_listen() to attempt bind and listen across candidates.
        4. Writes ns_last_error_string() to error_buffer if no candidate succeeds.
        5. Frees the getaddrinfo result list.

    Args:
        port:              Port to listen on as a decimal string.
        backlog:           Maximum pending connection queue length.
        error_buffer:      If non-NULL, receives a human-readable error on failure.
        error_buffer_size: Size of error_buffer in bytes.

    Returns:
        ns_socket_t: A listening socket on success, NS_INVALID_SOCKET on failure.
*/
ns_socket_t ns_listen_tcp(const char *port, int backlog, char *error_buffer, size_t error_buffer_size) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    ns_socket_t listen_socket = NS_INVALID_SOCKET;
    int status = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    status = getaddrinfo(NULL, port, &hints, &results);
    if(status != 0) {
        if(error_buffer != NULL && error_buffer_size > 0) {
#ifdef _WIN32
            snprintf(error_buffer, error_buffer_size, "%s", gai_strerrorA(status));
#else
            snprintf(error_buffer, error_buffer_size, "%s", gai_strerror(status));
#endif
        }
        return NS_INVALID_SOCKET;
    }

    listen_socket = ns_try_bind_listen(results, backlog);

    if(!ns_socket_is_valid(listen_socket) && error_buffer != NULL && error_buffer_size > 0) {
        ns_last_error_string(error_buffer, error_buffer_size);
    }

    freeaddrinfo(results);
    return listen_socket;
}

/* ns_packet_set - Initialize an NsPacket with the given metadata and body text.

    Automatically sets header.version to NS_PROTOCOL_VERSION. Rejects bodies
    longer than NS_PACKET_BODY_MAX so callers never need to pre-check length.

    At each step:
        1. Returns -1 if packet is NULL.
        2. Measures body length; returns -1 if it exceeds NS_PACKET_BODY_MAX.
        3. Zeroes the packet structure with memset.
        4. Populates all header fields including version.
        5. Copies the body and appends a null terminator.

    Args:
        packet:    Packet structure to populate; must not be NULL.
        type:      NsPacketType value (JOIN, TEXT, LEAVE, ACK, or ERROR).
        sender_id: Numeric user ID to embed in the header.
        timestamp: Unix timestamp for the packet (use ns_unix_time_now()).
        body:      Null-terminated body text; may be NULL for an empty body.

    Returns:
        int: 0 on success, -1 if packet is NULL or body exceeds NS_PACKET_BODY_MAX.
*/
int ns_packet_set(NsPacket *packet, uint8_t type, uint32_t sender_id, uint32_t timestamp, const char *body) {
    size_t body_length = 0;

    if(packet == NULL) {
        return -1;
    }

    if(body != NULL) {
        body_length = strlen(body);
        if (body_length > NS_PACKET_BODY_MAX) {
            return -1;
        }
    }

    memset(packet, 0, sizeof(*packet));
    packet->header.version = NS_PROTOCOL_VERSION;
    packet->header.type = type;
    packet->header.sender_id = sender_id;
    packet->header.timestamp = timestamp;
    packet->header.body_len = (uint32_t) body_length;

    if(body_length > 0) {
        memcpy(packet->body, body, body_length);
    }
    packet->body[body_length] = '\0';
    return 0;
}

/* ns_send_packet - Serialize and transmit a complete packet over socket_fd.

    Serializes the 14-byte header field-by-field in network byte order, then
    sends the body. Two ns_send_all calls are made (header then body) so no
    intermediate assembly buffer is needed for large bodies.

    At each step:
        1. Returns -1 if socket_fd is invalid, packet is NULL, or body_len is too large.
        2. Zeroes and populates the 14-byte header buffer.
        3. Calls ns_send_all() for the header; returns -1 on failure.
        4. Returns 0 immediately if body_len is 0.
        5. Calls ns_send_all() for the body and returns its result.

    Args:
        socket_fd: Connected socket to write to.
        packet:    Packet to transmit; body_len must be <= NS_PACKET_BODY_MAX.

    Returns:
        int: 0 on success, -1 on invalid arguments or send failure.
*/
int ns_send_packet(ns_socket_t socket_fd, const NsPacket *packet) {
    unsigned char header_buffer[NS_PACKET_HEADER_SIZE];

    if(!ns_socket_is_valid(socket_fd) || packet == NULL) {
        return -1;
    }
    if(packet->header.body_len > NS_PACKET_BODY_MAX) {
        return -1;
    }

    memset(header_buffer, 0, sizeof(header_buffer));
    header_buffer[0] = packet->header.version;
    header_buffer[1] = packet->header.type;
    ns_store_u32(header_buffer + 2,  packet->header.sender_id);
    ns_store_u32(header_buffer + 6,  packet->header.timestamp);
    ns_store_u32(header_buffer + 10, packet->header.body_len);

    if(ns_send_all(socket_fd, header_buffer, sizeof(header_buffer)) != 0) {
        return -1;
    }

    if(packet->header.body_len == 0) {
        return 0;
    }

    return ns_send_all(socket_fd, (const unsigned char *) packet->body, (size_t) packet->header.body_len);
}

/* ns_recv_packet - Read and validate a complete packet from socket_fd.

    Reads the 14-byte wire header, validates the protocol version and packet
    type before reading the body, then reads body_len bytes. Invalid bytes
    (unknown version or type) are rejected at the protocol layer so they
    never reach server business logic.

    At each step:
        1. Returns -1 if socket_fd is invalid or packet is NULL.
        2. Reads the 14-byte header with ns_recv_all(); returns recv_status on failure.
        3. Validates header.version against NS_PROTOCOL_VERSION; returns NS_RECV_ERROR on mismatch.
        4. Validates header.type against all known NsPacketType values; returns NS_RECV_ERROR for unknowns.
        5. Deserializes sender_id, timestamp, and body_len from the header buffer.
        6. Returns -1 if body_len exceeds NS_PACKET_BODY_MAX.
        7. Returns NS_RECV_OK immediately if body_len is 0.
        8. Reads body_len bytes with ns_recv_all(); returns recv_status on failure.
        9. Appends a null terminator and returns NS_RECV_OK.

    Args:
        socket_fd: Connected socket to read from.
        packet:    Destination structure; cleared before writing.

    Returns:
        int: NS_RECV_OK (1) on success, NS_RECV_CLOSED (0) on clean EOF before
        the header, NS_RECV_ERROR (-1) on protocol violation or receive error.
*/
int ns_recv_packet(ns_socket_t socket_fd, NsPacket *packet) {
    unsigned char header_buffer[NS_PACKET_HEADER_SIZE];
    int recv_status = 0;

    if(!ns_socket_is_valid(socket_fd) || packet == NULL) {
        return -1;
    }

    recv_status = ns_recv_all(socket_fd, header_buffer, sizeof(header_buffer));
    if(recv_status <= 0) {
        return recv_status;
    }

    memset(packet, 0, sizeof(*packet));
    packet->header.version = header_buffer[0];
    if(packet->header.version != NS_PROTOCOL_VERSION) {
        return NS_RECV_ERROR;
    }

    packet->header.type = header_buffer[1];

    switch(packet->header.type) {
        case NS_PACKET_JOIN:
        case NS_PACKET_TEXT:
        case NS_PACKET_LEAVE:
        case NS_PACKET_ACK:
        case NS_PACKET_ERROR:
            break;
        default:
            return NS_RECV_ERROR;
    }

    packet->header.sender_id = ns_load_u32(header_buffer + 2);
    packet->header.timestamp = ns_load_u32(header_buffer + 6);
    packet->header.body_len  = ns_load_u32(header_buffer + 10);

    if(packet->header.body_len > NS_PACKET_BODY_MAX) {
        return -1;
    }

    if(packet->header.body_len == 0) {
        packet->body[0] = '\0';
        return 1;
    }

    recv_status = ns_recv_all(socket_fd, (unsigned char *) packet->body, (size_t) packet->header.body_len);
    if(recv_status <= 0) {
        return recv_status;
    }

    packet->body[packet->header.body_len] = '\0';
    return 1;
}
