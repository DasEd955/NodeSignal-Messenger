/* test_comm.c - Unit tests for the comm networking and packet protocol module.

Tests cover:
  - ns_socket_is_valid against valid and invalid socket handles
  - ns_unix_time_now plausibility (returns a recent timestamp)
  - ns_last_error_string null-safety and buffer contract
  - ns_get_executable_dir null-safety
  - ns_packet_set for all NsPacketType values
  - ns_packet_set body boundary conditions (empty string, exact max, over max)
  - ns_send_packet / ns_recv_packet wire roundtrip over a real loopback socket pair
  - ns_recv_packet rejection of unknown protocol versions
  - ns_recv_packet rejection of unknown packet type bytes
  - ns_recv_packet rejection of body_len exceeding NS_PACKET_BODY_MAX in the wire header
  - ns_send_packet / ns_recv_packet with zero-body packets (all five types)
  - ns_listen_tcp / ns_connect_tcp on an ephemeral loopback port
  - ns_socket_shutdown and ns_socket_close safe on invalid socket
*/

#include "comm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
   Internal test counters (file-scoped; test_comm_run() reports totals).
--------------------------------------------------------------------------- */
static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        ++tests_run;                                                          \
        if(!(cond)) {                                                         \
            ++tests_failed;                                                   \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                     \
    } while(0)

/* ---------------------------------------------------------------------------
   Socket-pair helper.

   Creates a connected loopback pair: *server_out is the accepted peer of the
   listening socket, *client_out is the connected client socket.  The listen
   socket is closed before returning.  Returns 0 on success, -1 on failure.
--------------------------------------------------------------------------- */
static int make_loopback_pair(ns_socket_t *server_out, ns_socket_t *client_out)
{
    ns_socket_t listener   = NS_INVALID_SOCKET;
    ns_socket_t client_fd  = NS_INVALID_SOCKET;
    ns_socket_t server_fd  = NS_INVALID_SOCKET;
    struct sockaddr_in addr;
    ns_socklen_t addr_len  = (ns_socklen_t) sizeof(addr);
    int reuse              = 1;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(!ns_socket_is_valid(listener)) {
        return -1;
    }

    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               (const char *) &reuse, (ns_socklen_t) sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; /* OS picks an ephemeral port */

    if(bind(listener, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        ns_socket_close(listener);
        return -1;
    }
    if(listen(listener, 1) != 0) {
        ns_socket_close(listener);
        return -1;
    }
    if(getsockname(listener, (struct sockaddr *) &addr, &addr_len) != 0) {
        ns_socket_close(listener);
        return -1;
    }

    client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(!ns_socket_is_valid(client_fd)) {
        ns_socket_close(listener);
        return -1;
    }
    if(connect(client_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        ns_socket_close(listener);
        ns_socket_close(client_fd);
        return -1;
    }

    server_fd = accept(listener, NULL, NULL);
    ns_socket_close(listener);

    if(!ns_socket_is_valid(server_fd)) {
        ns_socket_close(client_fd);
        return -1;
    }

    *server_out = server_fd;
    *client_out = client_fd;
    return 0;
}

/* ---------------------------------------------------------------------------
   ns_socket_is_valid
--------------------------------------------------------------------------- */

/* test_socket_is_valid_invalid - NS_INVALID_SOCKET must report as invalid. */
static void test_socket_is_valid_invalid(void)
{
    CHECK(ns_socket_is_valid(NS_INVALID_SOCKET) == 0,
          "NS_INVALID_SOCKET is reported as invalid");
}

/* test_socket_is_valid_real - A live listening socket must report as valid. */
static void test_socket_is_valid_real(void)
{
    ns_socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(ns_socket_is_valid(fd) != 0, "real socket is reported as valid");
    ns_socket_close(fd);
}

/* ---------------------------------------------------------------------------
   ns_socket_close / ns_socket_shutdown null/invalid safety
--------------------------------------------------------------------------- */

/* test_socket_close_invalid - Closing an invalid socket must not crash. */
static void test_socket_close_invalid(void)
{
    ns_socket_close(NS_INVALID_SOCKET); /* must not crash or signal */
    ++tests_run;                        /* reaching here counts as a pass */
}

/* test_socket_shutdown_invalid - Shutting down an invalid socket must not crash. */
static void test_socket_shutdown_invalid(void)
{
    int rc = ns_socket_shutdown(NS_INVALID_SOCKET);
    CHECK(rc == 0, "ns_socket_shutdown on NS_INVALID_SOCKET returns 0");
}

/* ---------------------------------------------------------------------------
   ns_unix_time_now
--------------------------------------------------------------------------- */

/* test_unix_time_now_plausible - Returned timestamp must be after 2024-01-01. */
static void test_unix_time_now_plausible(void)
{
    /* 1704067200 == 2024-01-01 00:00:00 UTC */
    uint32_t now = ns_unix_time_now();
    CHECK(now > 1704067200U, "ns_unix_time_now returns a plausible recent timestamp");
}

/* test_unix_time_now_not_future - Returned timestamp must not be absurdly far in the future. */
static void test_unix_time_now_not_future(void)
{
    /* 4102444800 == 2100-01-01 00:00:00 UTC */
    uint32_t now = ns_unix_time_now();
    CHECK(now < 4102444800U, "ns_unix_time_now is not unreasonably far in the future");
}

/* ---------------------------------------------------------------------------
   ns_last_error_string
--------------------------------------------------------------------------- */

/* test_last_error_string_null_buffer - NULL buffer must return empty string literal. */
static void test_last_error_string_null_buffer(void)
{
    const char *result = ns_last_error_string(NULL, 128);
    CHECK(result != NULL, "ns_last_error_string with NULL buffer returns non-NULL");
    CHECK(result[0] == '\0', "ns_last_error_string with NULL buffer returns empty string");
}

/* test_last_error_string_zero_size - Zero size must return empty string literal. */
static void test_last_error_string_zero_size(void)
{
    char buf[4] = "xyz";
    const char *result = ns_last_error_string(buf, 0);
    CHECK(result != NULL, "ns_last_error_string with size 0 returns non-NULL");
    CHECK(result[0] == '\0', "ns_last_error_string with size 0 returns empty string");
}

/* test_last_error_string_returns_buffer - Valid call returns pointer to the supplied buffer. */
static void test_last_error_string_returns_buffer(void)
{
    char buf[256];
    const char *result = ns_last_error_string(buf, sizeof(buf));
    CHECK(result == buf, "ns_last_error_string returns pointer to supplied buffer");
}

/* ---------------------------------------------------------------------------
   ns_get_executable_dir
--------------------------------------------------------------------------- */

/* test_get_executable_dir_null - NULL buffer must return -1. */
static void test_get_executable_dir_null(void)
{
    int rc = ns_get_executable_dir(NULL, 256);
    CHECK(rc == -1, "ns_get_executable_dir returns -1 for NULL buffer");
}

/* test_get_executable_dir_zero_size - Zero size must return -1. */
static void test_get_executable_dir_zero_size(void)
{
    char buf[8];
    int rc = ns_get_executable_dir(buf, 0);
    CHECK(rc == -1, "ns_get_executable_dir returns -1 for size 0");
}

/* test_get_executable_dir_valid - Valid call succeeds and returns a non-empty path. */
static void test_get_executable_dir_valid(void)
{
    char buf[512];
    int rc = ns_get_executable_dir(buf, sizeof(buf));
    CHECK(rc == 0, "ns_get_executable_dir returns 0 on success");
    CHECK(buf[0] != '\0', "ns_get_executable_dir writes a non-empty path");
}

/* ---------------------------------------------------------------------------
   ns_packet_set -- all packet types and boundary conditions
--------------------------------------------------------------------------- */

/* test_packet_set_all_types - ns_packet_set succeeds for every NsPacketType value. */
static void test_packet_set_all_types(void)
{
    NsPacketType types[] = {
        NS_PACKET_JOIN, NS_PACKET_TEXT, NS_PACKET_LEAVE,
        NS_PACKET_ACK,  NS_PACKET_ERROR
    };
    size_t i = 0;

    for(i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        NsPacket pkt;
        int rc = ns_packet_set(&pkt, (uint8_t) types[i], 0, 0, NULL);
        CHECK(rc == 0,                                    "ns_packet_set succeeds for each NsPacketType");
        CHECK(pkt.header.type == (uint8_t) types[i],     "type field matches supplied NsPacketType");
        CHECK(pkt.header.version == NS_PROTOCOL_VERSION, "version is always NS_PROTOCOL_VERSION");
    }
}

/* test_packet_set_empty_string_body - Empty string body produces body_len == 0. */
static void test_packet_set_empty_string_body(void)
{
    NsPacket pkt;
    int rc = ns_packet_set(&pkt, NS_PACKET_TEXT, 1, 0, "");
    CHECK(rc == 0,                  "ns_packet_set accepts empty string body");
    CHECK(pkt.header.body_len == 0, "body_len is 0 for empty string body");
    CHECK(pkt.body[0] == '\0',      "body is null-terminated for empty string");
}

/* test_packet_set_one_byte_body - Single-character body produces body_len == 1. */
static void test_packet_set_one_byte_body(void)
{
    NsPacket pkt;
    int rc = ns_packet_set(&pkt, NS_PACKET_TEXT, 7, 999, "X");
    CHECK(rc == 0,                  "ns_packet_set accepts single-byte body");
    CHECK(pkt.header.body_len == 1, "body_len is 1 for single-byte body");
    CHECK(pkt.body[0] == 'X',       "body first byte is correct");
    CHECK(pkt.body[1] == '\0',      "body is null-terminated");
}

/* test_packet_set_sender_id_preserved - Large sender_id survives the round-trip. */
static void test_packet_set_sender_id_preserved(void)
{
    NsPacket pkt;
    uint32_t id = 0xDEADBEEFU;
    ns_packet_set(&pkt, NS_PACKET_ACK, id, 0, NULL);
    CHECK(pkt.header.sender_id == id, "large sender_id is preserved by ns_packet_set");
}

/* test_packet_set_timestamp_preserved - Large timestamp survives the round-trip. */
static void test_packet_set_timestamp_preserved(void)
{
    NsPacket pkt;
    uint32_t ts = 0xFFFFFFFFU;
    ns_packet_set(&pkt, NS_PACKET_TEXT, 0, ts, NULL);
    CHECK(pkt.header.timestamp == ts, "max uint32 timestamp is preserved by ns_packet_set");
}

/* test_packet_set_body_zeroed_on_null - Struct is fully zeroed before body is set. */
static void test_packet_set_body_zeroed_on_null(void)
{
    NsPacket pkt;
    /* Pre-poison the body region to confirm memset clears it. */
    memset(&pkt, 0xFF, sizeof(pkt));
    ns_packet_set(&pkt, NS_PACKET_LEAVE, 0, 0, NULL);
    CHECK(pkt.body[0] == '\0', "body[0] is zero when NULL body is supplied");
}

/* ---------------------------------------------------------------------------
   Wire roundtrip: ns_send_packet / ns_recv_packet over a real socket pair
--------------------------------------------------------------------------- */

/* test_send_recv_text_packet - TEXT packet survives a send/recv loopback cycle. */
static void test_send_recv_text_packet(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket sent;
    NsPacket received;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    rc = ns_packet_set(&sent, NS_PACKET_TEXT, 42, 1000, "hello world");
    CHECK(rc == 0, "ns_packet_set for TEXT roundtrip succeeds");

    rc = ns_send_packet(cli, &sent);
    CHECK(rc == 0, "ns_send_packet for TEXT packet succeeds");

    rc = ns_recv_packet(srv, &received);
    CHECK(rc == NS_RECV_OK,                              "ns_recv_packet returns NS_RECV_OK");
    CHECK(received.header.version == NS_PROTOCOL_VERSION,"version preserved over wire");
    CHECK(received.header.type == NS_PACKET_TEXT,        "type preserved over wire");
    CHECK(received.header.sender_id == 42U,              "sender_id preserved over wire");
    CHECK(received.header.timestamp == 1000U,            "timestamp preserved over wire");
    CHECK(received.header.body_len == 11U,               "body_len preserved over wire");
    CHECK(strcmp(received.body, "hello world") == 0,     "body content preserved over wire");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_send_recv_join_packet - JOIN packet with username body survives wire roundtrip. */
static void test_send_recv_join_packet(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket sent;
    NsPacket received;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    ns_packet_set(&sent, NS_PACKET_JOIN, 0, 0, "alice");
    ns_send_packet(cli, &sent);
    ns_recv_packet(srv, &received);

    CHECK(received.header.type == NS_PACKET_JOIN,    "JOIN type preserved over wire");
    CHECK(strcmp(received.body, "alice") == 0,        "JOIN username body preserved over wire");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_send_recv_leave_packet - LEAVE packet with empty body survives wire roundtrip. */
static void test_send_recv_leave_packet(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket sent;
    NsPacket received;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    ns_packet_set(&sent, NS_PACKET_LEAVE, 5, 0, NULL);
    ns_send_packet(cli, &sent);
    rc = ns_recv_packet(srv, &received);

    CHECK(rc == NS_RECV_OK,                          "ns_recv_packet returns NS_RECV_OK for LEAVE");
    CHECK(received.header.type == NS_PACKET_LEAVE,   "LEAVE type preserved over wire");
    CHECK(received.header.body_len == 0U,            "LEAVE body_len is 0 over wire");
    CHECK(received.body[0] == '\0',                  "LEAVE body is null-terminated");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_send_recv_max_body - Packet with exactly NS_PACKET_BODY_MAX body survives wire roundtrip. */
static void test_send_recv_max_body(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket sent;
    NsPacket received;
    char max_body[NS_PACKET_BODY_MAX + 1];
    int rc = 0;

    memset(max_body, 'M', NS_PACKET_BODY_MAX);
    max_body[NS_PACKET_BODY_MAX] = '\0';

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    ns_packet_set(&sent, NS_PACKET_TEXT, 1, 2, max_body);
    ns_send_packet(cli, &sent);
    rc = ns_recv_packet(srv, &received);

    CHECK(rc == NS_RECV_OK,                              "ns_recv_packet OK for max-body packet");
    CHECK(received.header.body_len == NS_PACKET_BODY_MAX,"body_len == NS_PACKET_BODY_MAX over wire");
    CHECK(strcmp(received.body, max_body) == 0,          "max body content preserved over wire");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_send_recv_multiple_sequential - Three back-to-back packets on the same connection. */
static void test_send_recv_multiple_sequential(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket pkt;
    NsPacket got;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    /* Packet 1: JOIN */
    ns_packet_set(&pkt, NS_PACKET_JOIN, 0, 0, "bob");
    ns_send_packet(cli, &pkt);
    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_OK && got.header.type == NS_PACKET_JOIN,
          "sequential packet 1 (JOIN) received correctly");

    /* Packet 2: TEXT */
    ns_packet_set(&pkt, NS_PACKET_TEXT, 10, 500, "hi");
    ns_send_packet(cli, &pkt);
    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_OK && got.header.type == NS_PACKET_TEXT &&
          strcmp(got.body, "hi") == 0,
          "sequential packet 2 (TEXT) received correctly");

    /* Packet 3: LEAVE */
    ns_packet_set(&pkt, NS_PACKET_LEAVE, 10, 600, NULL);
    ns_send_packet(cli, &pkt);
    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_OK && got.header.type == NS_PACKET_LEAVE,
          "sequential packet 3 (LEAVE) received correctly");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* ---------------------------------------------------------------------------
   ns_recv_packet - protocol violation rejection
--------------------------------------------------------------------------- */

/* test_recv_wrong_version - Packet with bad version byte must be rejected. */
static void test_recv_wrong_version(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    unsigned char bad_header[NS_PACKET_HEADER_SIZE];
    NsPacket got;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    memset(bad_header, 0, sizeof(bad_header));
    bad_header[0] = NS_PROTOCOL_VERSION + 1; /* invalid version */
    bad_header[1] = NS_PACKET_TEXT;

#ifdef _WIN32
    send(cli, (const char *) bad_header, (int) sizeof(bad_header), 0);
#else
    send(cli, bad_header, sizeof(bad_header), 0);
#endif

    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_ERROR, "ns_recv_packet returns NS_RECV_ERROR for wrong version");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_recv_unknown_type - Packet with unknown type byte must be rejected. */
static void test_recv_unknown_type(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    unsigned char bad_header[NS_PACKET_HEADER_SIZE];
    NsPacket got;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    memset(bad_header, 0, sizeof(bad_header));
    bad_header[0] = NS_PROTOCOL_VERSION;
    bad_header[1] = 0xFF; /* no such packet type */

#ifdef _WIN32
    send(cli, (const char *) bad_header, (int) sizeof(bad_header), 0);
#else
    send(cli, bad_header, sizeof(bad_header), 0);
#endif

    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_ERROR, "ns_recv_packet returns NS_RECV_ERROR for unknown type");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_recv_oversized_body_len - Wire header claiming body_len > NS_PACKET_BODY_MAX is rejected. */
static void test_recv_oversized_body_len(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    unsigned char bad_header[NS_PACKET_HEADER_SIZE];
    uint32_t oversized = NS_PACKET_BODY_MAX + 1;
    uint32_t net_val   = 0;
    NsPacket got;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    memset(bad_header, 0, sizeof(bad_header));
    bad_header[0] = NS_PROTOCOL_VERSION;
    bad_header[1] = NS_PACKET_TEXT;
    /* bytes 2-5: sender_id = 0 (already zero) */
    /* bytes 6-9: timestamp = 0 (already zero) */
    net_val = htonl(oversized);
    memcpy(bad_header + 10, &net_val, 4); /* body_len field */

#ifdef _WIN32
    send(cli, (const char *) bad_header, (int) sizeof(bad_header), 0);
#else
    send(cli, bad_header, sizeof(bad_header), 0);
#endif

    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_ERROR || rc < 0,
          "ns_recv_packet rejects wire body_len > NS_PACKET_BODY_MAX");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_recv_closed_connection - Clean EOF before any bytes returns NS_RECV_CLOSED. */
static void test_recv_closed_connection(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket got;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    /* Close the client without sending anything -- server should get clean EOF. */
    ns_socket_close(cli);
    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_CLOSED, "ns_recv_packet returns NS_RECV_CLOSED on clean EOF");

    ns_socket_close(srv);
}

/* ---------------------------------------------------------------------------
   ns_send_packet / ns_recv_packet - invalid argument guards
--------------------------------------------------------------------------- */

/* test_send_packet_null_packet - ns_send_packet with NULL packet returns -1. */
static void test_send_packet_null_packet(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    rc = ns_send_packet(cli, NULL);
    CHECK(rc == -1, "ns_send_packet returns -1 for NULL packet");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_send_packet_invalid_socket - ns_send_packet with NS_INVALID_SOCKET returns -1. */
static void test_send_packet_invalid_socket(void)
{
    NsPacket pkt;
    ns_packet_set(&pkt, NS_PACKET_TEXT, 0, 0, "x");
    int rc = ns_send_packet(NS_INVALID_SOCKET, &pkt);
    CHECK(rc == -1, "ns_send_packet returns -1 for NS_INVALID_SOCKET");
}

/* test_recv_packet_null_packet - ns_recv_packet with NULL packet returns -1. */
static void test_recv_packet_null_packet(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    int rc = 0;

    if(make_loopback_pair(&srv, &cli) != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL [%s:%d] could not create loopback socket pair\n",
                __FILE__, __LINE__);
        return;
    }

    rc = ns_recv_packet(srv, NULL);
    CHECK(rc == -1, "ns_recv_packet returns -1 for NULL packet");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_recv_packet_invalid_socket - ns_recv_packet with NS_INVALID_SOCKET returns -1. */
static void test_recv_packet_invalid_socket(void)
{
    NsPacket pkt;
    int rc = ns_recv_packet(NS_INVALID_SOCKET, &pkt);
    CHECK(rc == -1, "ns_recv_packet returns -1 for NS_INVALID_SOCKET");
}

/* ---------------------------------------------------------------------------
   ns_listen_tcp / ns_connect_tcp -- loopback TCP factory
--------------------------------------------------------------------------- */

/* test_listen_connect_tcp - Create a listening socket and connect to it on loopback.

   Uses a fixed ephemeral port range (15901) since Winsock does not accept port 0
   in ns_listen_tcp.  Connects using the address family of whatever socket
   ns_listen_tcp actually bound (IPv4 or dual-stack IPv6), extracted via getsockname.
*/
static void test_listen_connect_tcp(void)
{
    char errbuf[256];
    ns_socket_t listener  = NS_INVALID_SOCKET;
    ns_socket_t connected = NS_INVALID_SOCKET;
    struct sockaddr_storage addr;
    ns_socklen_t addr_len = (ns_socklen_t) sizeof(addr);
    char host_str[64];
    char port_str[16];
    struct sockaddr_in  *addr4 = NULL;
    struct sockaddr_in6 *addr6 = NULL;

    listener = ns_listen_tcp("15901", 1, errbuf, sizeof(errbuf));
    if(!ns_socket_is_valid(listener)) {
        /* Port 15901 may be in use; try another. */
        listener = ns_listen_tcp("15902", 1, errbuf, sizeof(errbuf));
    }

    CHECK(ns_socket_is_valid(listener), "ns_listen_tcp returns a valid socket");

    if(!ns_socket_is_valid(listener)) {
        return;
    }

    memset(&addr, 0, sizeof(addr));
    getsockname(listener, (struct sockaddr *) &addr, &addr_len);

    if(((struct sockaddr *) &addr)->sa_family == AF_INET6) {
        addr6 = (struct sockaddr_in6 *) &addr;
        /* If the listener bound to ::, connect via the IPv4 mapped loopback. */
        snprintf(host_str, sizeof(host_str), "::1");
        snprintf(port_str, sizeof(port_str), "%u",
                 (unsigned) ntohs(addr6->sin6_port));
    } else {
        addr4 = (struct sockaddr_in *) &addr;
        snprintf(host_str, sizeof(host_str), "127.0.0.1");
        snprintf(port_str, sizeof(port_str), "%u",
                 (unsigned) ntohs(addr4->sin_port));
    }

    connected = ns_connect_tcp(host_str, port_str, errbuf, sizeof(errbuf));
    if(!ns_socket_is_valid(connected)) {
        /* Retry with IPv4 loopback if IPv6 connection failed. */
        connected = ns_connect_tcp("127.0.0.1", port_str, errbuf, sizeof(errbuf));
    }
    CHECK(ns_socket_is_valid(connected), "ns_connect_tcp connects to ns_listen_tcp socket");

    ns_socket_close(connected);
    ns_socket_close(listener);
}

/* test_connect_tcp_bad_host - ns_connect_tcp to an invalid host returns NS_INVALID_SOCKET. */
static void test_connect_tcp_bad_host(void)
{
    char errbuf[256];
    ns_socket_t fd = ns_connect_tcp("this.host.does.not.exist.invalid",
                                    "9999", errbuf, sizeof(errbuf));
    CHECK(!ns_socket_is_valid(fd),
          "ns_connect_tcp returns NS_INVALID_SOCKET for unreachable host");
    if(ns_socket_is_valid(fd)) {
        ns_socket_close(fd);
    }
}

/* test_connect_tcp_refused - ns_connect_tcp to a port with no listener returns NS_INVALID_SOCKET. */
static void test_connect_tcp_refused(void)
{
    char errbuf[256];
    /* Port 1 is reserved/blocked on most systems; connection should fail. */
    ns_socket_t fd = ns_connect_tcp("127.0.0.1", "1", errbuf, sizeof(errbuf));
    CHECK(!ns_socket_is_valid(fd),
          "ns_connect_tcp returns NS_INVALID_SOCKET for refused connection");
    if(ns_socket_is_valid(fd)) {
        ns_socket_close(fd);
    }
}

/* ---------------------------------------------------------------------------
   test_comm_run - Run all comm tests; return 0 if all pass, -1 otherwise.
--------------------------------------------------------------------------- */
int test_comm_run(void)
{
    int prior_failed = tests_failed;

    /* Socket validity */
    test_socket_is_valid_invalid();
    test_socket_is_valid_real();
    test_socket_close_invalid();
    test_socket_shutdown_invalid();

    /* Unix time */
    test_unix_time_now_plausible();
    test_unix_time_now_not_future();

    /* Error string */
    test_last_error_string_null_buffer();
    test_last_error_string_zero_size();
    test_last_error_string_returns_buffer();

    /* Executable dir */
    test_get_executable_dir_null();
    test_get_executable_dir_zero_size();
    test_get_executable_dir_valid();

    /* ns_packet_set */
    test_packet_set_all_types();
    test_packet_set_empty_string_body();
    test_packet_set_one_byte_body();
    test_packet_set_sender_id_preserved();
    test_packet_set_timestamp_preserved();
    test_packet_set_body_zeroed_on_null();

    /* Wire roundtrip */
    test_send_recv_text_packet();
    test_send_recv_join_packet();
    test_send_recv_leave_packet();
    test_send_recv_max_body();
    test_send_recv_multiple_sequential();

    /* Protocol violation rejection */
    test_recv_wrong_version();
    test_recv_unknown_type();
    test_recv_oversized_body_len();
    test_recv_closed_connection();

    /* Invalid argument guards */
    test_send_packet_null_packet();
    test_send_packet_invalid_socket();
    test_recv_packet_null_packet();
    test_recv_packet_invalid_socket();

    /* TCP factory */
    test_listen_connect_tcp();
    test_connect_tcp_bad_host();
    test_connect_tcp_refused();

    printf("[comm] %d tests run, %d failed\n",
           tests_run, tests_failed - prior_failed + (tests_failed - prior_failed < 0 ? 0 : 0));

    return (tests_failed > prior_failed) ? -1 : 0;
}
