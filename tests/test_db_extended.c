/* test_db_extended.c - Extended unit tests for the SQLite persistence layer.

Complements test_db.c with tests that cover:
  - Null-pointer guards on every public ns_db_* function
  - ns_db_last_error on NULL and valid handles
  - ns_db_init_schema idempotency (safe to call twice on the same handle)
  - ns_db_recent_messages limit clamping (limit < 1 and limit > 500)
  - ns_db_recent_messages oldest-first ordering guarantee
  - ns_db_recent_messages limit respected (only N rows delivered)
  - Multi-message history with multiple senders
  - ns_db_insert_message with null body returns -1
  - ns_db_get_or_create_user with null username returns -1
  - ns_db_recent_messages with null callback returns -1
  - Timestamp round-trip fidelity through insert/retrieve cycle
  - Foreign-key constraint: inserting a message for a non-existent sender_id fails
*/

#include "db.h"
#include "comm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
   Callback helpers
--------------------------------------------------------------------------- */

/* NsOrderCapture - Stores up to 512 received rows in insertion order. */
typedef struct
{
    char   username[512][64];
    char   body[512][NS_PACKET_BODY_MAX + 1];
    int64_t sent_at[512];
    int     count;
} NsOrderCapture;

static void capture_callback(const char *username, const char *body,
                              int64_t sent_at, void *userdata)
{
    NsOrderCapture *cap = (NsOrderCapture *) userdata;
    if(cap->count >= 512) {
        return;
    }
    snprintf(cap->username[cap->count], 64,                  "%s", username);
    snprintf(cap->body[cap->count],     NS_PACKET_BODY_MAX + 1, "%s", body);
    cap->sent_at[cap->count] = sent_at;
    ++cap->count;
}

/* count_callback - Simply increments a counter on each invocation. */
static void count_callback(const char *username, const char *body,
                           int64_t sent_at, void *userdata)
{
    (void) username;
    (void) body;
    (void) sent_at;
    int *counter = (int *) userdata;
    ++(*counter);
}

/* ---------------------------------------------------------------------------
   Helper: open a fresh in-memory database and initialize schema.
   Returns 0 on success; increments failure counters and returns -1 on error.
--------------------------------------------------------------------------- */
static int open_test_db(NsDatabase *db)
{
    int rc = ns_db_open(db, ":memory:");
    if(rc != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL: could not open in-memory database\n");
        return -1;
    }
    rc = ns_db_init_schema(db);
    if(rc != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL: could not initialize in-memory schema\n");
        ns_db_close(db);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
   Null-pointer guard tests (no database needed)
--------------------------------------------------------------------------- */

/* test_db_open_null_database - ns_db_open with NULL database returns -1. */
static void test_db_open_null_database(void)
{
    int rc = ns_db_open(NULL, ":memory:");
    CHECK(rc == -1, "ns_db_open returns -1 for NULL database");
}

/* test_db_open_null_path - ns_db_open with NULL path returns -1. */
static void test_db_open_null_path(void)
{
    NsDatabase db;
    int rc = ns_db_open(&db, NULL);
    CHECK(rc == -1, "ns_db_open returns -1 for NULL path");
}

/* test_db_close_null - ns_db_close with NULL database does not crash. */
static void test_db_close_null(void)
{
    ns_db_close(NULL); /* must not crash */
    ++tests_run;       /* reaching here is the pass condition */
}

/* test_db_init_schema_null_database - ns_db_init_schema with NULL database returns -1. */
static void test_db_init_schema_null_database(void)
{
    int rc = ns_db_init_schema(NULL);
    CHECK(rc == -1, "ns_db_init_schema returns -1 for NULL database");
}

/* test_db_get_or_create_user_null_database - NULL database returns -1. */
static void test_db_get_or_create_user_null_database(void)
{
    uint32_t id = 0;
    int rc = ns_db_get_or_create_user(NULL, "alice", &id);
    CHECK(rc == -1, "ns_db_get_or_create_user returns -1 for NULL database");
}

/* test_db_get_or_create_user_null_username - NULL username returns -1. */
static void test_db_get_or_create_user_null_username(void)
{
    NsDatabase db;
    uint32_t id = 0;

    if(open_test_db(&db) != 0) {
        return;
    }
    int rc = ns_db_get_or_create_user(&db, NULL, &id);
    CHECK(rc == -1, "ns_db_get_or_create_user returns -1 for NULL username");
    ns_db_close(&db);
}

/* test_db_get_or_create_user_null_out_id - NULL out_user_id returns -1. */
static void test_db_get_or_create_user_null_out_id(void)
{
    NsDatabase db;

    if(open_test_db(&db) != 0) {
        return;
    }
    int rc = ns_db_get_or_create_user(&db, "alice", NULL);
    CHECK(rc == -1, "ns_db_get_or_create_user returns -1 for NULL out_user_id");
    ns_db_close(&db);
}

/* test_db_insert_message_null_database - NULL database returns -1. */
static void test_db_insert_message_null_database(void)
{
    int rc = ns_db_insert_message(NULL, 1, "body", 0);
    CHECK(rc == -1, "ns_db_insert_message returns -1 for NULL database");
}

/* test_db_insert_message_null_body - NULL body returns -1. */
static void test_db_insert_message_null_body(void)
{
    NsDatabase db;
    uint32_t user_id = 0;

    if(open_test_db(&db) != 0) {
        return;
    }
    ns_db_get_or_create_user(&db, "bob", &user_id);
    int rc = ns_db_insert_message(&db, user_id, NULL, 0);
    CHECK(rc == -1, "ns_db_insert_message returns -1 for NULL body");
    ns_db_close(&db);
}

/* test_db_recent_messages_null_database - NULL database returns -1. */
static void test_db_recent_messages_null_database(void)
{
    int counter = 0;
    int rc = ns_db_recent_messages(NULL, 10, count_callback, &counter);
    CHECK(rc == -1, "ns_db_recent_messages returns -1 for NULL database");
}

/* test_db_recent_messages_null_callback - NULL callback returns -1. */
static void test_db_recent_messages_null_callback(void)
{
    NsDatabase db;

    if(open_test_db(&db) != 0) {
        return;
    }
    int rc = ns_db_recent_messages(&db, 10, NULL, NULL);
    CHECK(rc == -1, "ns_db_recent_messages returns -1 for NULL callback");
    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   ns_db_last_error
--------------------------------------------------------------------------- */

/* test_db_last_error_null - ns_db_last_error on NULL returns a non-NULL, non-empty string. */
static void test_db_last_error_null(void)
{
    const char *msg = ns_db_last_error(NULL);
    CHECK(msg != NULL,    "ns_db_last_error returns non-NULL for NULL database");
    CHECK(msg[0] != '\0', "ns_db_last_error returns non-empty string for NULL database");
}

/* test_db_last_error_valid - ns_db_last_error on an open handle returns a non-NULL string. */
static void test_db_last_error_valid(void)
{
    NsDatabase db;
    const char *msg = NULL;

    if(open_test_db(&db) != 0) {
        return;
    }
    msg = ns_db_last_error(&db);
    CHECK(msg != NULL, "ns_db_last_error returns non-NULL for valid database");
    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   ns_db_init_schema idempotency
--------------------------------------------------------------------------- */

/* test_db_init_schema_idempotent - Calling ns_db_init_schema twice on the same handle succeeds. */
static void test_db_init_schema_idempotent(void)
{
    NsDatabase db;
    int rc1 = 0;
    int rc2 = 0;

    if(ns_db_open(&db, ":memory:") != 0) {
        ++tests_run;
        ++tests_failed;
        fprintf(stderr, "FAIL: could not open in-memory database for idempotency test\n");
        return;
    }

    rc1 = ns_db_init_schema(&db);
    rc2 = ns_db_init_schema(&db);

    CHECK(rc1 == 0, "first ns_db_init_schema call succeeds");
    CHECK(rc2 == 0, "second ns_db_init_schema call succeeds (idempotent)");

    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   ns_db_recent_messages -- limit clamping
--------------------------------------------------------------------------- */

/* test_recent_messages_limit_below_one - limit < 1 is clamped; function still returns 0. */
static void test_recent_messages_limit_below_one(void)
{
    NsDatabase db;
    int counter = 0;
    int rc = 0;

    if(open_test_db(&db) != 0) {
        return;
    }
    rc = ns_db_recent_messages(&db, 0, count_callback, &counter);
    CHECK(rc == 0, "ns_db_recent_messages returns 0 when limit is 0 (clamped)");
    ns_db_close(&db);
}

/* test_recent_messages_limit_above_500 - limit > 500 is clamped to 500; function returns 0. */
static void test_recent_messages_limit_above_500(void)
{
    NsDatabase db;
    int counter = 0;
    int rc = 0;

    if(open_test_db(&db) != 0) {
        return;
    }
    rc = ns_db_recent_messages(&db, 9999, count_callback, &counter);
    CHECK(rc == 0, "ns_db_recent_messages returns 0 when limit is 9999 (clamped to 500)");
    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   ns_db_recent_messages -- ordering and limit enforcement
--------------------------------------------------------------------------- */

/* test_recent_messages_oldest_first - Messages are delivered oldest-first regardless of DESC query. */
static void test_recent_messages_oldest_first(void)
{
    NsDatabase db;
    uint32_t user_id = 0;
    NsOrderCapture cap;
    int rc = 0;

    memset(&cap, 0, sizeof(cap));

    if(open_test_db(&db) != 0) {
        return;
    }

    rc = ns_db_get_or_create_user(&db, "chrono", &user_id);
    CHECK(rc == 0, "user creation for ordering test succeeds");

    ns_db_insert_message(&db, user_id, "first",  100U);
    ns_db_insert_message(&db, user_id, "second", 200U);
    ns_db_insert_message(&db, user_id, "third",  300U);

    rc = ns_db_recent_messages(&db, 10, capture_callback, &cap);
    CHECK(rc == 0,                                "ns_db_recent_messages returns 0");
    CHECK(cap.count == 3,                         "all three messages returned");
    CHECK(strcmp(cap.body[0], "first")  == 0,     "oldest message delivered first");
    CHECK(strcmp(cap.body[1], "second") == 0,     "middle message delivered second");
    CHECK(strcmp(cap.body[2], "third")  == 0,     "newest message delivered last");

    ns_db_close(&db);
}

/* test_recent_messages_limit_respected - Only the N most recent rows are delivered. */
static void test_recent_messages_limit_respected(void)
{
    NsDatabase db;
    uint32_t user_id = 0;
    int counter = 0;
    int rc = 0;

    if(open_test_db(&db) != 0) {
        return;
    }

    ns_db_get_or_create_user(&db, "limiter", &user_id);

    /* Insert 5 messages; request only 3. */
    ns_db_insert_message(&db, user_id, "msg1", 10U);
    ns_db_insert_message(&db, user_id, "msg2", 20U);
    ns_db_insert_message(&db, user_id, "msg3", 30U);
    ns_db_insert_message(&db, user_id, "msg4", 40U);
    ns_db_insert_message(&db, user_id, "msg5", 50U);

    rc = ns_db_recent_messages(&db, 3, count_callback, &counter);
    CHECK(rc == 0,      "ns_db_recent_messages returns 0 with limit 3");
    CHECK(counter == 3, "exactly 3 rows delivered when limit is 3");

    ns_db_close(&db);
}

/* test_recent_messages_most_recent_subset - With limit < total, the most recent N messages are returned. */
static void test_recent_messages_most_recent_subset(void)
{
    NsDatabase db;
    uint32_t user_id = 0;
    NsOrderCapture cap;
    int rc = 0;

    memset(&cap, 0, sizeof(cap));

    if(open_test_db(&db) != 0) {
        return;
    }

    ns_db_get_or_create_user(&db, "subset", &user_id);
    ns_db_insert_message(&db, user_id, "old",    10U);
    ns_db_insert_message(&db, user_id, "middle", 20U);
    ns_db_insert_message(&db, user_id, "recent", 30U);

    /* Request 2: should get "middle" then "recent" (oldest-first of the 2 most recent). */
    rc = ns_db_recent_messages(&db, 2, capture_callback, &cap);
    CHECK(rc == 0,                                  "ns_db_recent_messages returns 0 with limit 2");
    CHECK(cap.count == 2,                           "exactly 2 rows returned");
    CHECK(strcmp(cap.body[0], "middle") == 0,       "first delivered is the older of the 2 recent");
    CHECK(strcmp(cap.body[1], "recent") == 0,       "second delivered is the newest message");

    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   Multi-sender message history
--------------------------------------------------------------------------- */

/* test_multi_sender_history - Messages from two senders are joined with the correct usernames. */
static void test_multi_sender_history(void)
{
    NsDatabase db;
    uint32_t alice_id = 0;
    uint32_t bob_id   = 0;
    NsOrderCapture cap;
    int rc = 0;

    memset(&cap, 0, sizeof(cap));

    if(open_test_db(&db) != 0) {
        return;
    }

    rc = ns_db_get_or_create_user(&db, "alice", &alice_id);
    CHECK(rc == 0, "alice created");
    rc = ns_db_get_or_create_user(&db, "bob", &bob_id);
    CHECK(rc == 0, "bob created");

    ns_db_insert_message(&db, alice_id, "hello from alice", 1U);
    ns_db_insert_message(&db, bob_id,   "hello from bob",   2U);
    ns_db_insert_message(&db, alice_id, "alice again",       3U);

    rc = ns_db_recent_messages(&db, 10, capture_callback, &cap);
    CHECK(rc == 0,     "ns_db_recent_messages returns 0 for multi-sender history");
    CHECK(cap.count == 3, "all three messages retrieved");

    if(cap.count == 3) {
        CHECK(strcmp(cap.username[0], "alice") == 0, "first message sender is alice");
        CHECK(strcmp(cap.body[0], "hello from alice") == 0, "first message body is correct");
        CHECK(strcmp(cap.username[1], "bob") == 0,   "second message sender is bob");
        CHECK(strcmp(cap.username[2], "alice") == 0, "third message sender is alice again");
        CHECK(strcmp(cap.body[2], "alice again") == 0, "third message body is correct");
    }

    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   Timestamp fidelity
--------------------------------------------------------------------------- */

/* test_timestamp_roundtrip - sent_at delivered to callback matches what was inserted. */
static void test_timestamp_roundtrip(void)
{
    NsDatabase db;
    uint32_t user_id = 0;
    NsOrderCapture cap;
    uint32_t stored_ts = 0xDEAD1234U;

    memset(&cap, 0, sizeof(cap));

    if(open_test_db(&db) != 0) {
        return;
    }

    ns_db_get_or_create_user(&db, "tsuser", &user_id);
    ns_db_insert_message(&db, user_id, "ts test", stored_ts);
    ns_db_recent_messages(&db, 1, capture_callback, &cap);

    CHECK(cap.count == 1, "one message returned for timestamp test");
    if(cap.count == 1) {
        CHECK((uint32_t) cap.sent_at[0] == stored_ts,
              "timestamp round-trips correctly through insert/retrieve");
    }

    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   Empty body message
--------------------------------------------------------------------------- */

/* test_insert_empty_body - An empty body string is inserted and retrieved correctly. */
static void test_insert_empty_body(void)
{
    NsDatabase db;
    uint32_t user_id = 0;
    NsOrderCapture cap;
    int rc = 0;

    memset(&cap, 0, sizeof(cap));

    if(open_test_db(&db) != 0) {
        return;
    }

    ns_db_get_or_create_user(&db, "emptyuser", &user_id);
    rc = ns_db_insert_message(&db, user_id, "", 0U);
    CHECK(rc == 0, "ns_db_insert_message accepts empty string body");

    ns_db_recent_messages(&db, 1, capture_callback, &cap);
    CHECK(cap.count == 1,                  "empty-body message is retrievable");
    CHECK(cap.body[0][0] == '\0',          "retrieved body is empty string");

    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   Foreign-key enforcement
--------------------------------------------------------------------------- */

/* test_foreign_key_enforcement - Inserting a message with a nonexistent sender_id must fail. */
static void test_foreign_key_enforcement(void)
{
    NsDatabase db;
    int rc = 0;

    if(open_test_db(&db) != 0) {
        return;
    }

    /* sender_id 99999 was never inserted into users; FK constraint must reject it. */
    rc = ns_db_insert_message(&db, 99999U, "orphan message", 0U);
    CHECK(rc == -1,
          "ns_db_insert_message rejects message with non-existent sender_id (FK constraint)");

    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   No messages case
--------------------------------------------------------------------------- */

/* test_recent_messages_empty_table - Query on empty table invokes callback zero times and returns 0. */
static void test_recent_messages_empty_table(void)
{
    NsDatabase db;
    int counter = 0;
    int rc = 0;

    if(open_test_db(&db) != 0) {
        return;
    }

    rc = ns_db_recent_messages(&db, 10, count_callback, &counter);
    CHECK(rc == 0,      "ns_db_recent_messages returns 0 on empty table");
    CHECK(counter == 0, "callback not invoked on empty table");

    ns_db_close(&db);
}

/* ---------------------------------------------------------------------------
   test_db_extended_run - Run all extended database tests.
--------------------------------------------------------------------------- */
int test_db_extended_run(void)
{
    int prior_failed = tests_failed;

    /* Null-pointer guards */
    test_db_open_null_database();
    test_db_open_null_path();
    test_db_close_null();
    test_db_init_schema_null_database();
    test_db_get_or_create_user_null_database();
    test_db_get_or_create_user_null_username();
    test_db_get_or_create_user_null_out_id();
    test_db_insert_message_null_database();
    test_db_insert_message_null_body();
    test_db_recent_messages_null_database();
    test_db_recent_messages_null_callback();

    /* ns_db_last_error */
    test_db_last_error_null();
    test_db_last_error_valid();

    /* Schema idempotency */
    test_db_init_schema_idempotent();

    /* Limit clamping */
    test_recent_messages_limit_below_one();
    test_recent_messages_limit_above_500();

    /* Ordering and limit enforcement */
    test_recent_messages_oldest_first();
    test_recent_messages_limit_respected();
    test_recent_messages_most_recent_subset();

    /* Multi-sender */
    test_multi_sender_history();

    /* Timestamp fidelity */
    test_timestamp_roundtrip();

    /* Edge-case body */
    test_insert_empty_body();

    /* Foreign key enforcement */
    test_foreign_key_enforcement();

    /* Empty table */
    test_recent_messages_empty_table();

    printf("[db_extended] %d tests run, %d failed\n",
           tests_run, tests_failed - prior_failed + (tests_failed - prior_failed < 0 ? 0 : 0));

    return (tests_failed > prior_failed) ? -1 : 0;
}
