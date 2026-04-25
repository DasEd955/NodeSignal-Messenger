/* ===================================================================================
comm.c -- Implements the shared networking & packet protocol logic
    -- Initializes & cleans up the networking subsystem
    -- Provides cross-platform socket utilities
    -- Connects to & listens for TCP connections
    -- Builds, sends, and receives protocol packets
    -- Converts networking errors into readable messages
=================================================================================== */

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

#define NS_PACKET_HEADER_SIZE 16U

/* static void ns_store_u32 -- Stores a 32-bit unsigned integer into a byte buffer in network byte order

    -- Acts as a helper function for serializing 32-bit values into packet headers
    -- Used when preparing packet header fields before sending data across the network

    -- unsigned char *buffer: The byte buffer where the converted value will be stored
    -- uint32_t value: The 32-bit unsigned integer value to store

    -- Declares uint32_t network_value to hold the converted network byte order value
    -- Calls htonl() to convert value from host byte order to network byte order
    -- Calls memcpy() to copy the converted value into the destination buffer
    */
static void ns_store_u32(unsigned char *buffer, uint32_t value) {
    uint32_t network_value = htonl(value);
    memcpy(buffer, &network_value, sizeof(network_value));
}

/* static uint32_t ns_load_u32 -- Loads a 32-bit unsigned integer from a byte buffer & converts it to host byte order

    -- Acts as a helper function for deserializing 32-bit values from packet headers
    -- Used when reading packet header fields received from the network

    -- const unsigned char *buffer: The byte buffer containing the serialized 32-bit value

    -- Declares uint32_t network_value = 0 to store the raw value copied from the buffer
    -- Calls memcpy() to copy the 32-bit value from the buffer into network_value
    -- Calls ntohl() to convert the value from network byte order to host byte order
    -- Returns the converted 32-bit value
    */
static uint32_t ns_load_u32(const unsigned char *buffer) {
    uint32_t network_value = 0;
    memcpy(&network_value, buffer, sizeof(network_value));
    return ntohl(network_value);
}

/* static int ns_send_all -- Sends all bytes from a buffer over a socket connection

    -- Acts as a helper function for reliably sending an entire block of data
    -- Used when packet headers or packet bodies must be fully transmitted across the network

    -- ns_socket_t socket_fd: The socket used to send the data
    -- const unsigned char *buffer: The byte buffer containing the data to send
    -- size_t buffer_size: The total number of bytes that must be sent

    -- Declares size_t total_sent = 0 to track how many bytes have already been sent

    -- Loops while total_send is less than buffer_size
    -- Calls send() to transmit the remaining unsent bytes
        -- On Windows, uses int sent_now
        -- On Unix/Linux, uses ssize_t sent_now
    -- If send() returns 0 or a negative value:
        -- Returns -1 to indicate failure
    -- Otherwise:
        -- Adds the number of bytes sent to total_sent
    
    -- Returns 0 after all bytes in the buffer have been sent successfully
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

/* static int ns_recv_all -- Receives an exact number of bytes from a socket connection 

    -- Acts as a helper function for reliably reading a complete block of data
    -- Used when packet headers or packet bodies must be fully received from the network

    -- ns_socket_t socket_fd: The socket used to receive the data
    -- unsigned char *buffer: The byte buffer where the received data will be stored
    -- size_t buffer_size: The exact number of bytes that must be received 

    -- Declares size_t total_received to track how many bytes have already been received

    -- Loops while total_received is less than buffer_size
    -- Calls recv() to read the remaining bytes
        -- On Windows, uses int received_now
        -- On Unix/Linux, uses ssize_t received_now
    
    -- If recv() returns 0:
        -- Returns 0 if no bytes were received at all, meaning the connection was closed cleanly
        -- Returns -1 if only part of the requested data was received before the connection closed

    -- If recv() returns a negative value:
        -- Returns -1 to indicate failure
    -- Otherwise:
        -- Add the number of bytes received to total_received
    
    -- Returns 1 after the full requested number of bytes has been received successfully
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

/* int ns_net_init -- Initializes the network subsystem for the program

    -- Acts as a public communication function for preparing the network layer beforer socket operations
    -- Used before creating, listening on, or sending data through sockets

    -- On Windows:
        -- Declares WSADATA data to store Windock initialization data
        -- Calls WSAStartup() with version 2.2 to initialize Winsock
        -- Returns the result of WSAStartup()
    
    -- On Unix/Linux:
        -- No specialize network initialization is required
        -- Returns 0
    */
int ns_net_init(void) {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
#else
    return 0;
#endif
}

/* void ns_net_cleanup -- Cleans up the networking subsystem before program exit

    -- Acts as a public communication function for releasing networking resources
    -- Used after the program is finished performing socket operations

    -- On Windows:
        -- Calls WSACleanup() to clean up Winsock resources
    
    -- On Unix/Linux:
        -- No special network cleanup is required
    */
void ns_net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

/* void ns_socket_close -- Closes a socket if it is valid

    -- Acts as a public communication function for safely closing a socket
    -- Used when the program is finished using a socket connection

    -- ns_socket_t socket_fd: The socket to close

    -- If ns_socket_is_valid() reports that the socket is invalid:
        -- Returns immediately
    
    -- On Windows:
        -- Calls closesocket() to close the socket
    
    -- On Unix/Linux:
        -- Calls close() system call to close the socket
    */
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

/* int ns_socket_shutdown -- Shuts down communication on a socket if it is valid

    -- Acts as a public communication function for stopping sends & receives on a socket
    -- Used before closing a socket connection or when the program wants to terminate communication cleanly

    -- ns_socket_t socket_fd: The socket to shut down

    -- If ns_socket_is_valid() reports that the socket is invalid:
        -- Returns 0 immediately
    
    -- On Windows:
        -- Calls shutdown() with SD_BOTH to disable both sending & receiving 
        -- Returns the result of shutdown()

    -- On Unix/Linux:
        -- Calls shutdown with SHUT_RDWR to disable both sending & receiving 
        -- Returns the result of shutdown()
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

/* int ns_socket_is_valid -- Checks whether a socket handle is invalid

    -- Acts as a public communication function for validating socket handles
    -- Used before performing operations such as send, receive, shutdown, or close
    
    -- ns_socket_t socket_fd: The socket handle being checked

    -- Compares socket_fd against NS_INVALID_SOCKET
        -- Returns nonzero if the socket is valid
        -- Returns zero if the socket is invalid
    */
int ns_socket_is_valid(ns_socket_t socket_fd) {
    return socket_fd != NS_INVALID_SOCKET;
}

/* uint32_t ns_unix_time_now -- Returns the current Unix timestamp

    -- Acts as a public communication utility function for retrieving the current time
    -- Used when packet timestamps or database timestamps are needed

    -- Declares time_t now & stores the result of time(NULL)
    -- If time(NULL) returns a negative value:
        -- Returns 0U
    -- Otherwise:
        -- Casts the current time to uint32_t
        -- Returns the Unix timestamp in seconds
    */
uint32_t ns_unix_time_now(void) {
    time_t now = time(NULL);
    if(now < 0) {
        return 0U;
    }
    return (uint32_t) now;
}

/* int ns_get_executable_dir -- Retrieves the directory containing the current executable

    -- Acts as a public communication utility function for locating runtime files relative to the program
    -- Used by the client and server when they need to load assets or database files from an installed package

    -- char *buffer: The character buffer where the executable directory will be written
    -- size_t buffer_size: The size of buffer in bytes

    -- If buffer is NULL or buffer_size is 0:
        -- Returns -1

    -- On Windows:
        -- Calls GetModuleFileNameA() to retrieve the full executable path
    -- On Unix/Linux:
        -- Calls readlink() on /proc/self/exe to retrieve the full executable path

    -- Finds the last path separator in the executable path
    -- Replaces that separator with a null terminator so buffer stores only the directory path
    -- Returns 0 on success or -1 on failure
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

/* const char *ns_last_error_string -- Retrieves the most recent system or socket error message as readable text

    -- Acts as a public communication utility function for converting the latest error into a readable string
    -- Used when the program needs to report networking or system errors to the console or user

    -- char *buffer: The character buffer where the error message will be written
    -- size_t buffer_size: The size of the buffer in bytes

    -- If the buffer is NULL or buffer_size is 0:
        -- Returns an empty string
    
    -- On Windows:
        -- Declares DWORD error_code to store the result of WSAGetLastError()
        -- Declares DWORD copied to store the number of characters written by FormatMessageA()
        -- Calls WSAGetLastError() to get the latest Winsock error code 
        -- Calls FormatMessageA() to convert that error code into a readable message in buffer
        -- If FormatMessageA() fails:
            -- Uses snprintf() to write a fallback error message into buffer
    
    -- On Unix/Linux:
        -- Calls strerror(errno) to get the latest system error string
        -- Uses snprintf() to copy that error string into buffer
    
    -- Returns buffer
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

// DEV NOTE: Consider splitting into helpers to improve readability
/* ns_socket_t ns_connect_tcp -- Connects to a remote TCP server & returns the connected socket 

    -- Acts as a communication function for creating an outgoing TCP client connection
    -- Used by the client to connect to the server on the requested host & port
    
    -- const char *host: The hostname or IP address of the remote server
    -- const char *port: The port number of the remote server
    -- char *error_buffer: Buffer used to store a readable error message if the connection fails
    -- size_t error_buffer_size: The size of the error_buffer in bytes
    
    -- Declares struct addrinfo hints to describe the type of socket addresses being requested
    -- Declares struct addrinfo *results = NULL to store the linked list returned by getaddrinfo()
    -- Declares struct addrinfo *candidate = NULL to iterate through each candidate address
    -- Declares ns_socket_t socket_fd = NS_INVALID_SOCKET to store the connected socket
    -- Declares int status = 0 to store the return value from getaddrinfo()

    -- Clears hints with memset()
    -- Configures hints for:
        -- AF_UNSPEC so that either IPv4 or IPv6 may be used
        -- SOCK_STREAM so that a TCP stream socket is requested
        -- IPPROTO_TCP so that the TCP protocol is used

    -- Calls getaddrinfo() to resolve host & port into candidate socket addresses
    -- If getaddrinfo() fails:
        -- If error_buffer is valid, writes a readable getaddrinfo() error message into it
        -- Returns NS_INVALID_SOCKET

    -- Loops through each candidate address returned by getaddrinfo()
        -- Calls socket() to create a socket for the current candidate
        -- If the socket is invalid, continues to the next candidate
        -- Calls connect() to attempt a connection using the current candidate
        -- If connect() succeeds, stop searching
        -- If connect() fails:
            -- Closes the socket
            -- Resets socket_fd to NS_INVALID_SOCKET
            -- Continues to the next candidate
    
    -- If no candidate produced a valid connected socket & error_buffer is valid:
        -- Calls ns_last_error_string() to store a readable socket error message in error_buffer
    
    -- Calls freeaddrinfo() to release the candidate address list
    -- Returns the connected socket on success or NS_INVALID_SOCKET on failure
    */
ns_socket_t ns_connect_tcp(const char *host, const char *port, char *error_buffer, size_t error_buffer_size) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *candidate = NULL;
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

    for(candidate = results; candidate != NULL; candidate = candidate->ai_next) {
        socket_fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if(!ns_socket_is_valid(socket_fd)) {
            continue;
        }

        if(connect(socket_fd, candidate->ai_addr, (ns_socklen_t) candidate->ai_addrlen) == 0) {
            break;
        }

        ns_socket_close(socket_fd);
        socket_fd = NS_INVALID_SOCKET;
    }

    if(!ns_socket_is_valid(socket_fd) && error_buffer != NULL && error_buffer_size > 0) {
        ns_last_error_string(error_buffer, error_buffer_size);
    }

    freeaddrinfo(results);
    return socket_fd;
}

// DEV NOTE: Consider splitting into helpers to improve readability
/* ns_socket_t ns_listen_tcp -- Creates a TCP listening socket on the given port 

    -- Acts as a public communication function for creating a server listening socket
    -- Used by the server to bind to a port & begin accepting incoming TCP connections
    
    -- const char *port: The port number the server should listen on
    -- int backlog: The max number of pending connection requests
    -- char *error_buffer: Buffer used the store a readable error message if socket setup fails
    -- size_t error_buffer_size: The size of error_buffer in bytes

    -- Declares struct addrinfo hints to describe the type of socket addresses being requested
    -- Declares struct addrinfo *results = NULL to store the linked list returned by getaddrinfo()
    -- Declares struct addrinfo *candidate = NULL to iterate through each candidate address
    -- Declares ns_socket_t listen_socket = NS_INVALID_SOCKET to store the listening socket
    -- Declares int status = 0 to store the return value from getaddrinfo()
    -- Declares int reuse = 1 to enable address reuse on the listening socket
    
    -- Clears hints with memset()
    -- Configures hints for:
        -- AF_UNSPEC so either IPv4 or IPv6 may be used
        -- SOCK_STREAM so a TCP stream socket is requested
        -- IPPROTO_TCP so the TCP protocol is used
        -- AI_PASSIVE so the address is suitable for bind()
    
    -- Calls getaddrinfo() to resolve the local port into candidate bind addresses
    -- If getaddrinfo() fails:
        -- If error_buffer is valid, writes a readable getaddrinfo() error message into it
        -- Returns NS_INVALID_SOCKET

    -- Loops through each candidate address returned by getaddrinfo()
        -- Calls socket() to create a socket for the current candidate
        -- If the socket is invalid, continues to the next candidate
        -- Calls setsockopt() with SO_REUSEADDR to allow address reuse
        -- Calls bind() to bind the socket to the current candidate address
        -- If bind() fails:
            -- Closes the socket
            -- Resets listen_socket to NS_INVALID_SOCKET
            -- Continues to the next candidate
        -- Calls listen() to place the socket into listening mode
        -- If listen() succeeds, stops searching
        -- If listen() fails:
            -- Closes the socket
            -- Resets listen_socket to NS_INVALID_SOCKET
            -- Continues to the next candidate
    
    -- If no candidate produced a valid listening socket & error_buffer is valid:
        -- Calls ns_last_error_string() to store a readable socket error message in error_buffer
    
    -- Calls freeaddrinfo() to release the candidate address list
    -- Returns the listening socket upon success or NS_INVALID_SOCKET upon failure
    */
ns_socket_t ns_listen_tcp(const char *port, int backlog, char *error_buffer, size_t error_buffer_size) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *candidate = NULL;
    ns_socket_t listen_socket = NS_INVALID_SOCKET;
    int status = 0;
    int reuse = 1;

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

    for(candidate = results; candidate != NULL; candidate = candidate->ai_next) {
        int dual_stack = 0;

        listen_socket = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if(!ns_socket_is_valid(listen_socket)) {
            continue;
        }

        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, (ns_socklen_t) sizeof(reuse));

#ifdef _WIN32
        if(candidate->ai_family == AF_INET6) {
            dual_stack = 0;
            setsockopt(listen_socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &dual_stack,
                       (ns_socklen_t) sizeof(dual_stack));
        }
#endif

        if(bind(listen_socket, candidate->ai_addr, (ns_socklen_t) candidate->ai_addrlen) != 0) {
            ns_socket_close(listen_socket);
            listen_socket = NS_INVALID_SOCKET;
            continue;
        }

        if(listen(listen_socket, backlog) == 0) {
            break;
        }

        ns_socket_close(listen_socket);
        listen_socket = NS_INVALID_SOCKET;
    }

    if(!ns_socket_is_valid(listen_socket) && error_buffer != NULL && error_buffer_size > 0) {
        ns_last_error_string(error_buffer, error_buffer_size);
    }

    freeaddrinfo(results);
    return listen_socket;
}

/* int ns_packet_set -- Initializes a packet with the given header value & body text

    -- Acts as a public communication function for preparing a packet before it is sent across the network
    -- Used to fill in packet header fields & safely copy the packet body text

    -- NsPacket *packet: The packet structure to initialize
    -- uint8_t type: The packet type to store in the header
    -- uint32_t sender_id: The sender ID to store in the header
    -- uint32_t timestamp: The Unix timestamp to store in the header
    -- const char *body: The packet body text to copy into the packet
    
    -- Declares size_t body_length = 0 to store the length of the body text

    -- If packet is NULL:
        -- Returns -1
    
    -- If body is not NULL:
        -- Calls strlen() to determine the body length
        -- If body_length is greater than NS_PACKET_BODY_MAX:
            -- Returns -1
    
    -- Clears the packet structure with memset()
    -- Stores type, sender_id, timestamp, and body_length in the packet

    -- If body_length is greater than 0:
        -- Calls memcpy() to copy the body text into packet->body
    
    -- Writes a null terminator at packet->body[body_length]
    -- Returns 0 upon success
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

/* int ns_send_packet -- Sends a complete packet over a socket connection 

    -- Acts as a public communication function for transmitting a full protocol packet 
    -- Used to send both the fixed-size packet header & the packet body across the network

    -- ns_socket_t socket_fd: The socket used to send the packet
    -- const NsPacket *packet: The packet structure containing the header fields & body text to send

    -- Declares unsigned char header_buffer[NS_PACKET_HEADER_SIZE] to store the serialized packet header

    -- If ns_socket_is_valid() reports that socket_fd is invalid or packet is NULL:
        -- Returns -1 
    
    -- If packet->header.body_len is greater than NS_PACKET_BODY_MAX:
        -- Returns -1
    
    -- Clears header_buffer with memset()
    -- Stores the packet type in header_buffer[0]
    -- Calls ns_store_u32() to serialize sender_id, timestamp, and body_len into the header buffer

    -- Calls ns_send_all() to send the fixed-size header buffer
    -- If sending the header fails:
        -- Returns -1
    
    -- If packet->header.body_len is 0:
        -- Returns 0 since there is no body to send
    
    -- Otherwise:
        -- Calls ns_send_all() to send the pcket body
        -- Returns the result of sending the body
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
    header_buffer[0] = packet->header.type;
    ns_store_u32(header_buffer + 4, packet->header.sender_id);
    ns_store_u32(header_buffer + 8, packet->header.timestamp);
    ns_store_u32(header_buffer + 12, packet->header.body_len);

    if(ns_send_all(socket_fd, header_buffer, sizeof(header_buffer)) != 0) {
        return -1;
    }

    if(packet->header.body_len == 0) {
        return 0;
    }

    return ns_send_all(socket_fd, (const unsigned char *) packet->body, (size_t) packet->header.body_len);
}

/* int ns_recv_packet -- Receives a complete packet from a socket connection 

    -- Acts as a public communication function for reading a full protocol packet from the network
    -- Used to receive both the fixed-size packet header & the packet body from a socket

    -- ns_socket_t socket_fd: The socket used to receive the packet
    -- NsPacket *packet: The packet structure where the received header & body data will be stored

    -- Declares unsigned char header_buffer[NS_PACKET_HEADER_SIZE] to store the raw received header bytes
    -- Declares int recv_status = 0 to store the result returned by ns_recv_all()

    -- If ns_socket_is_valid() reports that socket_fd is invalid or packet is NULL:
        -- Returns -1
    
    -- Calls ns_recv_all() to read the fixed-size packet header into header_buffer
    -- If ns_recv_all() returns 0 or -1:
        -- Returns recv_status immediately 
    
    -- Clears the packet structure with memset()
    -- Reads the packet type from header_buffer[0]
    -- Calls ns_load_u32() to deserialize sender_id, timestamp, and body_len from the header_buffer

    -- If packet->header.body_len is greater than NS_PACKET_BODY_MAX:
        -- Returns -1
    
    -- If packet->header.body_len is 0:
        -- Stores the null terminator in packet->body[0]
        -- Returns 1
    
    -- Calls ns_recv_all() to read the packet body into packet->body
    -- If ns_recv_all() returns 0 or -1:
        -- Returns recv_status immediately
    
    -- Writes a null terminator at the end of the received body text
    -- Returns 1 upon success
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
    packet->header.type = header_buffer[0];
    packet->header.sender_id = ns_load_u32(header_buffer + 4);
    packet->header.timestamp = ns_load_u32(header_buffer + 8);
    packet->header.body_len = ns_load_u32(header_buffer + 12);

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
