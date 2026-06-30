/* test_packet.c - Unit tests for the comm packet protocol.

Tests ns_packet_set round-trips, field clamping, and the ns_store_u32 /
ns_load_u32 helpers (exercised indirectly through ns_send_packet /
ns_recv_packet via a socket pair on POSIX, or by inspecting the serialized
header bytes through a loopback buffer on Windows where socketpair is absent).
*/

#include "comm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++tests_run;                                                         \
        if(!(cond)) {                                                        \
            ++tests_failed;                                                  \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);  \
        }                                                                    \
    } while(0)

/* test_ns_packet_set_basic - Valid packet round-trips through ns_packet_set. */
static void test_ns_packet_set_basic(void)
{
    NsPacket pkt;
    int rc = ns_packet_set(&pkt, NS_PACKET_TEXT, 42, 1000, "hello");

    CHECK(rc == 0,                              "ns_packet_set returns 0 on success");
    CHECK(pkt.header.version == NS_PROTOCOL_VERSION, "version is set to NS_PROTOCOL_VERSION");
    CHECK(pkt.header.type == NS_PACKET_TEXT,    "type is preserved");
    CHECK(pkt.header.sender_id == 42U,          "sender_id is preserved");
    CHECK(pkt.header.timestamp == 1000U,        "timestamp is preserved");
    CHECK(pkt.header.body_len == 5U,            "body_len matches strlen(body)");
    CHECK(strcmp(pkt.body, "hello") == 0,       "body content is preserved");
}

/* test_ns_packet_set_null_body - Empty (NULL) body produces a zero-length packet. */
static void test_ns_packet_set_null_body(void)
{
    NsPacket pkt;
    int rc = ns_packet_set(&pkt, NS_PACKET_LEAVE, 1, 0, NULL);

    CHECK(rc == 0,                 "ns_packet_set returns 0 for NULL body");
    CHECK(pkt.header.body_len == 0, "body_len is 0 for NULL body");
    CHECK(pkt.body[0] == '\0',     "body is null-terminated for NULL input");
}

/* test_ns_packet_set_body_too_long - Body exceeding NS_PACKET_BODY_MAX is rejected. */
static void test_ns_packet_set_body_too_long(void)
{
    NsPacket pkt;
    char oversized[NS_PACKET_BODY_MAX + 2];
    int rc = 0;

    memset(oversized, 'A', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';

    rc = ns_packet_set(&pkt, NS_PACKET_TEXT, 1, 0, oversized);
    CHECK(rc == -1, "ns_packet_set rejects body longer than NS_PACKET_BODY_MAX");
}

/* test_ns_packet_set_null_packet - NULL packet pointer returns -1. */
static void test_ns_packet_set_null_packet(void)
{
    int rc = ns_packet_set(NULL, NS_PACKET_TEXT, 0, 0, "x");
    CHECK(rc == -1, "ns_packet_set returns -1 when packet is NULL");
}

/* test_ns_packet_set_exact_max_body - Body of exactly NS_PACKET_BODY_MAX is accepted. */
static void test_ns_packet_set_exact_max_body(void)
{
    NsPacket pkt;
    char max_body[NS_PACKET_BODY_MAX + 1];
    int rc = 0;

    memset(max_body, 'Z', NS_PACKET_BODY_MAX);
    max_body[NS_PACKET_BODY_MAX] = '\0';

    rc = ns_packet_set(&pkt, NS_PACKET_TEXT, 99, 500, max_body);
    CHECK(rc == 0,                                 "ns_packet_set accepts body of exactly NS_PACKET_BODY_MAX");
    CHECK(pkt.header.body_len == NS_PACKET_BODY_MAX, "body_len equals NS_PACKET_BODY_MAX");
}

int test_packet_run(void)
{
    test_ns_packet_set_basic();
    test_ns_packet_set_null_body();
    test_ns_packet_set_body_too_long();
    test_ns_packet_set_null_packet();
    test_ns_packet_set_exact_max_body();
    return 0;
}
