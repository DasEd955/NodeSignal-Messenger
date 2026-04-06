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

typedef enum NsPacketType {
    NS_PACKET_JOIN = 1,
    NS_PACKET_TEXT = 2,
    NS_PACKET_LEAVE = 3,
    NS_PACKET_ACK = 4,
    NS_PACKET_ERROR = 5
} NsPacketType;

typedef struct NsPacketHeader {
    uint8_t type;
    uint32_t sender_id;
    uint32_t timestamp;
    uint32_t body_len;
} NsPacketHeader;

typedef struct NsPacket {
    NsPacketHeader header;
    char body[NS_PACKET_BODY_MAX + 1U];
} NsPacket;

int ns_net_init(void);
void ns_net_cleanup(void);
void ns_socket_close(ns_socket_t socket_fd);
int ns_socket_shutdown(ns_socket_t socket_fd);
int ns_socket_is_valid(ns_socket_t socket_fd);
uint32_t ns_unix_time_now(void);
const char *ns_last_error_string(char *buffer, size_t buffer_size);

ns_socket_t ns_connect_tcp(const char *host,
                           const char *port,
                           char *error_buffer,
                           size_t error_buffer_size);

ns_socket_t ns_listen_tcp(const char *port,
                          int backlog,
                          char *error_buffer,
                          size_t error_buffer_size);

int ns_packet_set(NsPacket *packet,
                  uint8_t type,
                  uint32_t sender_id,
                  uint32_t timestamp,
                  const char *body);

int ns_send_packet(ns_socket_t socket_fd, const NsPacket *packet);
int ns_recv_packet(ns_socket_t socket_fd, NsPacket *packet);

#ifdef __cplusplus
}
#endif

#endif
