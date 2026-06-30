/* test_integration.c - Integration tests for the NodeSignal comm protocol.

These tests exercise the full wire protocol end-to-end over real loopback TCP
sockets.  They do NOT start the actual server process; instead they create a
raw listening socket on one side and a connected client socket on the other,
then drive the exchange manually.  This validates that the serialisation and
deserialisation logic in comm.c is self-consistent and that the 14-byte wire
format is correctly round-tripped across a genuine kernel socket boundary.

Test cases:
  - ACK packet roundtrip (server-side packet type)
  - ERROR packet roundtrip with a body message
  - Bidirectional exchange: client sends JOIN, server replies ACK
  - Bidirectional exchange: client sends TEXT, server echoes TEXT
  - Bidirectional exchange: client sends LEAVE, server reads LEAVE
  - Large body fidelity: all 512 bytes preserved across the wire
  - Back-pressure: 10 TEXT packets sent and received sequentially
  - Wire header byte order: verify big-endian layout in the raw bytes
  - sender_id preservation across all five packet types
  - Protocol integrity: partial header write is not confused with a valid packet
*/

#include "comm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

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
   Loopback socket pair helper (duplicated here so this file compiles standalone)
--------------------------------------------------------------------------- */
static int make_loopback_pair(ns_socket_t *server_out, ns_socket_t *client_out)
{
    ns_socket_t listener  = NS_INVALID_SOCKET;
    ns_socket_t client_fd = NS_INVALID_SOCKET;
    ns_socket_t server_fd = NS_INVALID_SOCKET;
    struct sockaddr_in addr;
    ns_socklen_t addr_len = (ns_socklen_t) sizeof(addr);
    int reuse = 1;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(!ns_socket_is_valid(listener)) {
        return -1;
    }

    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               (const char *) &reuse, (ns_socklen_t) sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if(bind(listener, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
       listen(listener, 1) != 0 ||
       getsockname(listener, (struct sockaddr *) &addr, &addr_len) != 0) {
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

/* Macro to skip a test when the socket pair cannot be created. */
#define REQUIRE_PAIR(srv, cli)                                                \
    do {                                                                      \
        if(make_loopback_pair(&(srv), &(cli)) != 0) {                        \
            ++tests_run;                                                      \
            ++tests_failed;                                                   \
            fprintf(stderr, "FAIL [%s:%d] could not create loopback pair\n", \
                    __FILE__, __LINE__);                                      \
            return;                                                           \
        }                                                                     \
    } while(0)

/* ---------------------------------------------------------------------------
   Basic roundtrip tests for packet types not covered in test_comm.c
--------------------------------------------------------------------------- */

/* test_ack_roundtrip - ACK packet with user_id in sender_id and no body survives wire. */
static void test_ack_roundtrip(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket sent;
    NsPacket got;
    int rc = 0;

    REQUIRE_PAIR(srv, cli);

    /* Server sends ACK with the assigned user_id packed into sender_id. */
    rc = ns_packet_set(&sent, NS_PACKET_ACK, 77U, 0U, NULL);
    CHECK(rc == 0, "ns_packet_set for ACK succeeds");

    ns_send_packet(srv, &sent);
    rc = ns_recv_packet(cli, &got);

    CHECK(rc == NS_RECV_OK,                       "ns_recv_packet returns NS_RECV_OK for ACK");
    CHECK(got.header.type == NS_PACKET_ACK,       "ACK type preserved");
    CHECK(got.header.sender_id == 77U,            "ACK sender_id (user_id) preserved");
    CHECK(got.header.body_len == 0U,              "ACK body_len is 0");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_error_roundtrip - ERROR packet with body text survives the wire. */
static void test_error_roundtrip(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket sent;
    NsPacket got;
    const char *err_msg = "username already in use";
    int rc = 0;

    REQUIRE_PAIR(srv, cli);

    rc = ns_packet_set(&sent, NS_PACKET_ERROR, 0U, 0U, err_msg);
    CHECK(rc == 0, "ns_packet_set for ERROR succeeds");

    ns_send_packet(srv, &sent);
    rc = ns_recv_packet(cli, &got);

    CHECK(rc == NS_RECV_OK,                          "ns_recv_packet returns NS_RECV_OK for ERROR");
    CHECK(got.header.type == NS_PACKET_ERROR,        "ERROR type preserved");
    CHECK(got.header.body_len == strlen(err_msg),    "ERROR body_len matches message length");
    CHECK(strcmp(got.body, err_msg) == 0,            "ERROR body text preserved");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* ---------------------------------------------------------------------------
   Bidirectional protocol exchange tests
--------------------------------------------------------------------------- */

/* test_join_ack_exchange - Client sends JOIN; server reads it and replies ACK. */
static void test_join_ack_exchange(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket join_pkt;
    NsPacket ack_pkt;
    NsPacket recv_join;
    NsPacket recv_ack;
    int rc = 0;

    REQUIRE_PAIR(srv, cli);

    /* Step 1: client -> server: JOIN with username "delta" */
    rc = ns_packet_set(&join_pkt, NS_PACKET_JOIN, 0U, 0U, "delta");
    CHECK(rc == 0, "client ns_packet_set JOIN succeeds");
    ns_send_packet(cli, &join_pkt);

    rc = ns_recv_packet(srv, &recv_join);
    CHECK(rc == NS_RECV_OK,                        "server receives JOIN");
    CHECK(recv_join.header.type == NS_PACKET_JOIN, "server sees JOIN type");
    CHECK(strcmp(recv_join.body, "delta") == 0,    "server sees JOIN username");

    /* Step 2: server -> client: ACK with assigned user_id = 5 */
    rc = ns_packet_set(&ack_pkt, NS_PACKET_ACK, 5U, 0U, NULL);
    CHECK(rc == 0, "server ns_packet_set ACK succeeds");
    ns_send_packet(srv, &ack_pkt);

    rc = ns_recv_packet(cli, &recv_ack);
    CHECK(rc == NS_RECV_OK,                      "client receives ACK");
    CHECK(recv_ack.header.type == NS_PACKET_ACK, "client sees ACK type");
    CHECK(recv_ack.header.sender_id == 5U,       "client sees assigned user_id from ACK");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_text_echo_exchange - Client sends TEXT; server reads and echoes it back. */
static void test_text_echo_exchange(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket text_pkt;
    NsPacket echo_pkt;
    NsPacket recv_text;
    NsPacket recv_echo;
    const char *msg = "integration test message";
    int rc = 0;

    REQUIRE_PAIR(srv, cli);

    ns_packet_set(&text_pkt, NS_PACKET_TEXT, 3U, 42U, msg);
    ns_send_packet(cli, &text_pkt);

    rc = ns_recv_packet(srv, &recv_text);
    CHECK(rc == NS_RECV_OK,                             "server receives TEXT");
    CHECK(recv_text.header.type == NS_PACKET_TEXT,      "server sees TEXT type");
    CHECK(recv_text.header.sender_id == 3U,             "server sees correct sender_id");
    CHECK(recv_text.header.timestamp == 42U,            "server sees correct timestamp");
    CHECK(strcmp(recv_text.body, msg) == 0,             "server sees correct message body");

    /* Echo back to client (simulating a broadcast). */
    ns_packet_set(&echo_pkt, NS_PACKET_TEXT, 3U, 42U, "delta: integration test message");
    ns_send_packet(srv, &echo_pkt);

    rc = ns_recv_packet(cli, &recv_echo);
    CHECK(rc == NS_RECV_OK,                             "client receives echo TEXT");
    CHECK(recv_echo.header.type == NS_PACKET_TEXT,      "client sees TEXT type in echo");
    CHECK(strcmp(recv_echo.body, "delta: integration test message") == 0,
          "client sees formatted echo body");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* test_leave_exchange - Client sends LEAVE; server reads and confirms type. */
static void test_leave_exchange(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket leave_pkt;
    NsPacket recv_leave;
    int rc = 0;

    REQUIRE_PAIR(srv, cli);

    ns_packet_set(&leave_pkt, NS_PACKET_LEAVE, 7U, 0U, NULL);
    ns_send_packet(cli, &leave_pkt);

    rc = ns_recv_packet(srv, &recv_leave);
    CHECK(rc == NS_RECV_OK,                         "server receives LEAVE");
    CHECK(recv_leave.header.type == NS_PACKET_LEAVE,"server sees LEAVE type");
    CHECK(recv_leave.header.sender_id == 7U,        "server sees correct sender_id in LEAVE");
    CHECK(recv_leave.header.body_len == 0U,         "LEAVE body_len is 0");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* ---------------------------------------------------------------------------
   Large body fidelity
--------------------------------------------------------------------------- */

/* test_large_body_fidelity - 512 bytes of repeating pattern survive the wire intact. */
static void test_large_body_fidelity(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket sent;
    NsPacket got;
    char body[NS_PACKET_BODY_MAX + 1];
    int rc = 0;
    size_t i = 0;

    /* Fill with a recognisable non-zero repeating pattern. */
    for(i = 0; i < NS_PACKET_BODY_MAX; ++i) {
        body[i] = (char) ('A' + (i % 26));
    }
    body[NS_PACKET_BODY_MAX] = '\0';

    REQUIRE_PAIR(srv, cli);

    rc = ns_packet_set(&sent, NS_PACKET_TEXT, 1U, 1U, body);
    CHECK(rc == 0, "ns_packet_set accepts 512-byte body");
    ns_send_packet(cli, &sent);

    rc = ns_recv_packet(srv, &got);
    CHECK(rc == NS_RECV_OK,                         "ns_recv_packet OK for 512-byte body");
    CHECK(got.header.body_len == NS_PACKET_BODY_MAX,"body_len == 512 over wire");
    CHECK(memcmp(got.body, body, NS_PACKET_BODY_MAX) == 0,
          "all 512 body bytes match across the wire");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* ---------------------------------------------------------------------------
   Back-pressure: many packets in sequence
--------------------------------------------------------------------------- */

/* test_sequential_10_packets - 10 TEXT packets sent back-to-back are all received correctly. */
static void test_sequential_10_packets(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket pkt;
    NsPacket got;
    char body[32];
    int i = 0;
    int all_ok = 1;

    REQUIRE_PAIR(srv, cli);

    for(i = 0; i < 10; ++i) {
        snprintf(body, sizeof(body), "packet %d", i);
        ns_packet_set(&pkt, NS_PACKET_TEXT, (uint32_t) i, (uint32_t) (i * 100), body);
        ns_send_packet(cli, &pkt);

        int rc = ns_recv_packet(srv, &got);
        char expected[32];
        snprintf(expected, sizeof(expected), "packet %d", i);

        if(rc != NS_RECV_OK || got.header.sender_id != (uint32_t) i ||
           strcmp(got.body, expected) != 0) {
            all_ok = 0;
        }
    }

    CHECK(all_ok, "10 sequential TEXT packets all received correctly");

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* ---------------------------------------------------------------------------
   Wire header byte order verification
--------------------------------------------------------------------------- */

/* test_wire_header_big_endian - Inspect raw bytes to confirm big-endian layout. */
static void test_wire_header_big_endian(void)
{
    ns_socket_t srv = NS_INVALID_SOCKET;
    ns_socket_t cli = NS_INVALID_SOCKET;
    NsPacket pkt;
    unsigned char raw[NS_PACKET_HEADER_SIZE];
    int rc = 0;

    /* Use a sender_id with distinct bytes: 0x01020304 */
    const uint32_t test_sender_id  = 0x01020304U;
    const uint32_t test_timestamp  = 0xAABBCCDDU;

    REQUIRE_PAIR(srv, cli);

    ns_packet_set(&pkt, NS_PACKET_TEXT, test_sender_id, test_timestamp, NULL);
    ns_send_packet(cli, &pkt);

    /* Read exactly 14 raw header bytes before the recv layer touches them. */
#ifdef _WIN32
    rc = recv(srv, (char *) raw, (int) sizeof(raw), MSG_WAITALL);
#else
    rc = (int) recv(srv, raw, sizeof(raw), MSG_WAITALL);
#endif

    CHECK(rc == NS_PACKET_HEADER_SIZE, "14 raw header bytes received");

    if(rc == NS_PACKET_HEADER_SIZE) {
        CHECK(raw[0] == NS_PROTOCOL_VERSION, "wire byte [0] is version");
        CHECK(raw[1] == NS_PACKET_TEXT,      "wire byte [1] is type");

        /* sender_id at offset 2: big-endian 0x01020304 */
        CHECK(raw[2] == 0x01U, "sender_id byte[0] (MSB) is 0x01");
        CHECK(raw[3] == 0x02U, "sender_id byte[1] is 0x02");
        CHECK(raw[4] == 0x03U, "sender_id byte[2] is 0x03");
        CHECK(raw[5] == 0x04U, "sender_id byte[3] (LSB) is 0x04");

        /* timestamp at offset 6: big-endian 0xAABBCCDD */
        CHECK(raw[6]  == 0xAAU, "timestamp byte[0] (MSB) is 0xAA");
        CHECK(raw[7]  == 0xBBU, "timestamp byte[1] is 0xBB");
        CHECK(raw[8]  == 0xCCU, "timestamp byte[2] is 0xCC");
        CHECK(raw[9]  == 0xDDU, "timestamp byte[3] (LSB) is 0xDD");

        /* body_len at offset 10: 0 because body is NULL */
        CHECK(raw[10] == 0x00U, "body_len byte[0] is 0x00");
        CHECK(raw[11] == 0x00U, "body_len byte[1] is 0x00");
        CHECK(raw[12] == 0x00U, "body_len byte[2] is 0x00");
        CHECK(raw[13] == 0x00U, "body_len byte[3] is 0x00");
    }

    ns_socket_close(srv);
    ns_socket_close(cli);
}

/* ---------------------------------------------------------------------------
   sender_id preservation across all five packet types
--------------------------------------------------------------------------- */

/* test_sender_id_all_types - sender_id round-trips correctly for JOIN/TEXT/LEAVE/ACK/ERROR. */
static void test_sender_id_all_types(void)
{
    struct {
        NsPacketType type;
        uint32_t     sender_id;
        const char  *body;
    } cases[] = {
        { NS_PACKET_JOIN,  0x00000001U, "user1"  },
        { NS_PACKET_TEXT,  0x0000007FU, "hello"  },
        { NS_PACKET_LEAVE, 0x0000FFFFU, NULL     },
        { NS_PACKET_ACK,   0xDEADBEEFU, NULL     },
        { NS_PACKET_ERROR, 0x12345678U, "error"  },
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    size_t i = 0;

    for(i = 0; i < n; ++i) {
        ns_socket_t srv = NS_INVALID_SOCKET;
        ns_socket_t cli = NS_INVALID_SOCKET;
        NsPacket sent;
        NsPacket got;
        int rc = 0;

        if(make_loopback_pair(&srv, &cli) != 0) {
            ++tests_run;
            ++tests_failed;
            fprintf(stderr, "FAIL [%s:%d] loopback pair failed for type %d\n",
                    __FILE__, __LINE__, (int) cases[i].type);
            continue;
        }

        ns_packet_set(&sent, (uint8_t) cases[i].type,
                      cases[i].sender_id, 0U, cases[i].body);
        ns_send_packet(cli, &sent);
        rc = ns_recv_packet(srv, &got);

        CHECK(rc == NS_RECV_OK,
              "ns_recv_packet OK for sender_id test");
        CHECK(got.header.sender_id == cases[i].sender_id,
              "sender_id preserved for each packet type");

        ns_socket_close(srv);
        ns_socket_close(cli);
    }
}

/* ---------------------------------------------------------------------------
   test_integration_run - Run all integration tests; return 0 if all pass.
--------------------------------------------------------------------------- */
int test_integration_run(void)
{
    int prior_failed = tests_failed;

    test_ack_roundtrip();
    test_error_roundtrip();
    test_join_ack_exchange();
    test_text_echo_exchange();
    test_leave_exchange();
    test_large_body_fidelity();
    test_sequential_10_packets();
    test_wire_header_big_endian();
    test_sender_id_all_types();

    printf("[integration] %d tests run, %d failed\n",
           tests_run, tests_failed - prior_failed + (tests_failed - prior_failed < 0 ? 0 : 0));

    return (tests_failed > prior_failed) ? -1 : 0;
}
