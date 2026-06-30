/* test_db.c - Unit tests for the SQLite persistence layer.

Tests ns_db_get_or_create_user idempotency: calling it twice with the same
username must return the same user ID both times. Also tests that
ns_db_insert_message and ns_db_recent_messages round-trip a stored message.
*/

#include "db.h"
#include "comm.h"

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

/* Callback that captures the most recent (username, body) pair for assertions. */
typedef struct
{
    char username[64];
    char body[NS_PACKET_BODY_MAX + 1];
    int  call_count;
} NsTestCallbackState;

static void test_message_callback(const char *username, const char *body,
                                   int64_t sent_at, void *userdata)
{
    NsTestCallbackState *state = (NsTestCallbackState *) userdata;
    (void) sent_at;
    snprintf(state->username, sizeof(state->username), "%s", username);
    snprintf(state->body,     sizeof(state->body),     "%s", body);
    ++state->call_count;
}

/* test_get_or_create_user_idempotent - Same username yields same ID on repeated calls. */
static void test_get_or_create_user_idempotent(NsDatabase *db)
{
    uint32_t id1 = 0;
    uint32_t id2 = 0;
    int rc1 = ns_db_get_or_create_user(db, "testuser", &id1);
    int rc2 = ns_db_get_or_create_user(db, "testuser", &id2);

    CHECK(rc1 == 0,  "first ns_db_get_or_create_user call succeeds");
    CHECK(rc2 == 0,  "second ns_db_get_or_create_user call succeeds");
    CHECK(id1 == id2, "repeated calls return the same user ID");
    CHECK(id1 > 0,   "returned user ID is non-zero");
}

/* test_different_users_get_distinct_ids - Different usernames produce different IDs. */
static void test_different_users_get_distinct_ids(NsDatabase *db)
{
    uint32_t id_alice = 0;
    uint32_t id_bob = 0;
    int rc1 = ns_db_get_or_create_user(db, "alice", &id_alice);
    int rc2 = ns_db_get_or_create_user(db, "bob",   &id_bob);

    CHECK(rc1 == 0,             "alice creation succeeds");
    CHECK(rc2 == 0,             "bob creation succeeds");
    CHECK(id_alice != id_bob,   "alice and bob have distinct IDs");
}

/* test_insert_and_retrieve_message - Stored message is returned by ns_db_recent_messages. */
static void test_insert_and_retrieve_message(NsDatabase *db)
{
    uint32_t sender_id = 0;
    NsTestCallbackState state;
    int rc = 0;

    memset(&state, 0, sizeof(state));

    rc = ns_db_get_or_create_user(db, "msguser", &sender_id);
    CHECK(rc == 0, "user creation for message test succeeds");

    rc = ns_db_insert_message(db, sender_id, "hello world", 1000U);
    CHECK(rc == 0, "ns_db_insert_message returns 0 on success");

    rc = ns_db_recent_messages(db, 10, test_message_callback, &state);
    CHECK(rc == 0,                              "ns_db_recent_messages returns 0 on success");
    CHECK(state.call_count >= 1,                "callback was invoked at least once");
    CHECK(strcmp(state.body, "hello world") == 0, "retrieved message body matches inserted body");
    CHECK(strcmp(state.username, "msguser") == 0, "retrieved username matches sender");
}

int test_db_run(void)
{
    NsDatabase db;
    int rc = 0;

    /* Use an in-memory database so tests are isolated and leave no files. */
    rc = ns_db_open(&db, ":memory:");
    if(rc != 0) {
        ++tests_failed;
        ++tests_run;
        fprintf(stderr, "FAIL: could not open in-memory database\n");
        return -1;
    }

    rc = ns_db_init_schema(&db);
    if(rc != 0) {
        ++tests_failed;
        ++tests_run;
        fprintf(stderr, "FAIL: could not initialize in-memory schema\n");
        ns_db_close(&db);
        return -1;
    }

    test_get_or_create_user_idempotent(&db);
    test_different_users_get_distinct_ids(&db);
    test_insert_and_retrieve_message(&db);

    ns_db_close(&db);
    return 0;
}
