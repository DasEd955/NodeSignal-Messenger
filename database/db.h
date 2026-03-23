#ifndef NS_DB_H
#define NS_DB_H

#include <stdint.h>

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NsDatabase {
    sqlite3 *handle;
} NsDatabase;

int ns_db_open(NsDatabase *database, const char *path);
void ns_db_close(NsDatabase *database);
int ns_db_init_schema(NsDatabase *database);
int ns_db_get_or_create_user(NsDatabase *database,
                             const char *username,
                             uint32_t *out_user_id);
int ns_db_insert_message(NsDatabase *database,
                         uint32_t sender_id,
                         const char *body,
                         uint32_t timestamp);
const char *ns_db_last_error(const NsDatabase *database);

#ifdef __cplusplus
}
#endif

#endif
