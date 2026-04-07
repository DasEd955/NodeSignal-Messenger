/* ===================================================================================
comm.h -- Declares the public interface for the shared networking & packet protocol module
=================================================================================== */

#ifndef NS_COMM_H
#define NS_COMM_H

// Implements Functionality for Non-Windows Systems
    // Enables POSIX & common system declarations before system headers are included
    // Helps expose networking related functions & types on Unix/Linux Systems
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Platform Specific Networking Setup (Windows & Unix/Linux)
    // Creates a shared cross-platform interface so that the program can utilize socket data types without caring whether it is compiled on Windows or Linux
// For Windows:
    // Includes Winsock headers
    // Defines ns_socket_t as SOCKET
    // Defines ns_socklen_t as int
    // Defines NS_INVALID_SOCKET using Windows INVALID_SOCKET
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ns_socket_t;
typedef int ns_socklen_t;
#define NS_INVALID_SOCKET INVALID_SOCKET
// For Unix/Linux:
    // Includes POSIX socket headers
    // Defines ns_socket_t as int
    // Defines ns_socklen_t as socklen_t
    // Defines NS_INVALID_SOCKET as -1
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int ns_socket_t;
typedef socklen_t ns_socklen_t;
#define NS_INVALID_SOCKET (-1)
#endif

// For C++ Compatibility
    // If a C++ compiler includes this header, it treats the function declarations as C functions
    // Prevents C++ name mangling & lets C and C++ link properly
#ifdef __cplusplus
extern "C" {
#endif

#define NS_PACKET_BODY_MAX 512U
#define NS_USERNAME_MAX 32U

/* typedef enum NsPacketType -- Represents the different packet types used by the messaging protocol

    -- NS_PACKET_JOIN: Packet type used when a client requests to join the chat
    -- NS_PACKET_TEXT: Packet type used when a client sends a chat message
    -- NS_PACKET_LEAVE: Packet type used when a client leaves the chat
    -- NS_PACKET_ACK: Packet type used when the server acknowledges a successful join
    -- NS_PACKET_ERROR: Packet type used when the server reports an error to a client
    */
typedef enum NsPacketType {
    NS_PACKET_JOIN = 1,
    NS_PACKET_TEXT = 2,
    NS_PACKET_LEAVE = 3,
    NS_PACKET_ACK = 4,
    NS_PACKET_ERROR = 5
} NsPacketType;

/* typedef struct NsPacketHeader -- Represents the fixed size header (metadata) portion of a protocol packet

    -- uint8_t type: The packet type, such as JOIN, TEXT, LEAVE, ACK, or ERROR
    -- uint32_t sender_id: The numeric ID of the user associated with the packet
    -- uint32_t timestamp: The Unix timestamp for when the packet was created
    -- uint32_t body_len: The length of the packet body in bytes 
    */
typedef struct NsPacketHeader {
    uint8_t type;
    uint32_t sender_id;
    uint32_t timestamp;
    uint32_t body_len;
} NsPacketHeader;

/* typedef struct NsPacket -- Represents a complete protocol packet containing a header & message body

    -- NsPacketHeader header: The fixed size header containing packet metadata
    -- char body[NS_PACKET_BODY_MAX + 1U]: The packet body as a C string, including room for the null terminator 
    */
typedef struct NsPacket {
    NsPacketHeader header;
    char body[NS_PACKET_BODY_MAX + 1U];
} NsPacket;

/* Return codes used by ns_recv_packet(). */
enum {
    NS_RECV_ERROR = -1,
    NS_RECV_CLOSED = 0,
    NS_RECV_OK = 1
};

/* int ns_net_init -- Initializes the networking subsystem for the program

    -- Used before performing socket operations
    -- On Windows, initializes Winsock
    -- On Unix/Linux, returns success without further setup
    */
int ns_net_init(void);

/* void ns_net_cleanup -- Cleans up the networking subsystem before program exit

    -- Used after the program is finished performing socket operations
    -- On Windows, cleans up Winsock resources
    -- On Unix/Linux, no extra cleanup is required
    */
void ns_net_cleanup(void);

/* void ns_socket_close -- Closes a socket if it is valid

    -- ns_socket_t socket_fd: The socket to close
    -- Used to release a socket after the program is done using it
    */
void ns_socket_close(ns_socket_t socket_fd);

/* int ns_socket_shutdown -- Shuts down a socket connection if it is valid

    -- ns_socket_t socket_fd: The socket to shut down
    -- Used to stop communication on a socket before it is closed
    */
int ns_socket_shutdown(ns_socket_t socket_fd);

/* int ns_socket_is_valid -- Checks whether a socket handle is valid

    -- ns_socket_t socket_fd: The socket handle being checked
    -- Returns nonzero if the socket is valid; returns zero if the socket is invalid
    */
int ns_socket_is_valid(ns_socket_t socket_fd);

/* uint32_t ns_unix_time_now -- Returns the current Unix timestamp

    -- Used when the program needs the current time for packets or database records
    -- Returns the current time as the number of seconds since January 1, 1970
    */
uint32_t ns_unix_time_now(void);

/* const char *ns_last_error_string -- Retrieves the most recent system or socket error message as a string

    -- char *buffer: The character buffer where the error message will be written
    -- size_t buffer_size: The size of the buffer in bytes

    -- Used when the program needs a readable description of the most recent networking or system error
    -- Returns a pointer to the error message string stored in the buffer
    */
const char *ns_last_error_string(char *buffer, size_t buffer_size);

/* ns_socket_t ns_connect_tcp -- Connects to a remote TCP server & returns the connected socket 

    -- const char *host: The hostname or IP address of the remote sever
    -- const char *port: The port number of the remote server
    -- char *error_buffer: Buffer used to store readable error message if the connection fails
    -- size_t error_buffer_size: The size of error_buffer in bytes

    -- Used by the client to establish a TCP connection to the server
    -- Returns a valid socket upon success or NS_INVALID_SOCKET upon failure
    */
ns_socket_t ns_connect_tcp(const char *host, const char *port, char *error_buffer, size_t error_buffer_size);

/* ns_socket_t ns_listen_tcp -- Creates a TCP listening socket on the given port

    -- const char *port: The port number the server should listen on
    -- int backlog: The maximum number of pending connection requests
    -- char *error_buffer: Buffer used to store readable error message if the setup fails
    -- size_t error_buffer_size: The size of error_buffer in bytes

    -- Used by the server to create & start a listening TCP socket
    -- Returns a valid listening socket upon success or NS_INVALID_SOCKET upon failure
    */
ns_socket_t ns_listen_tcp(const char *port, int backlog, char *error_buffer, size_t error_buffer_size);

/* int ns_packet_set -- Initializes a packet with the given header values and body text

    -- NsPacket *packet: The packet structure to initialize
    -- uint8_t type: The packet type to store in the header
    -- uint32_t sender_id: The sender ID to store in the header
    -- uint32_t timestamp: The Unix timestamp to store in the header
    -- const char *body: The body text to copy into the packet
    
    -- Used to prepare packets before sending them across the network
    -- Returns 0 upon success or -1 upon failure
    */
int ns_packet_set(NsPacket *packet, uint8_t type, uint32_t sender_id, uint32_t timestamp, const char *body);

/* int ns_send_packet -- Sends a packet over a socket connection

    -- ns_socket_t socket_fd: The socket used to send the packet
    -- const NsPacket *packet: The packet to send

    -- Used to transmit a complete packet, including its header & body, across the network
    -- Returns 0 upon success or -1 upon failure
    */
int ns_send_packet(ns_socket_t socket_fd, const NsPacket *packet);

/* int ns_recv_packet -- Receives a packet from a socket connection

    -- ns_socket_t socket_fd: The socket used the receive the packet
    -- NsPacket *packet: The packet structure where the received data will be stored

    -- Used to read a complete packet, including its header & body, from the network
    -- Returns NS_RECV_OK upon success, NS_RECV_CLOSED if the connection was closed cleanly, or NS_RECV_ERROR upon failure
    */
int ns_recv_packet(ns_socket_t socket_fd, NsPacket *packet);

#ifdef __cplusplus
}
#endif

#endif
