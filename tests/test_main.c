/* test_main.c - Entry point for the NodeSignal unit test suite. */

#include "comm.h"

#include <stdio.h>

extern int test_packet_run(void);
extern int test_db_run(void);

int main(void)
{
    int exit_code = 0;

    ns_net_init();

    if(test_packet_run() != 0) {
        exit_code = 1;
    }
    if(test_db_run() != 0) {
        exit_code = 1;
    }

    ns_net_cleanup();

    if(exit_code == 0) {
        puts("All tests passed.");
    }
    return exit_code;
}
