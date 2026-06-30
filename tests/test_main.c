/* test_main.c - Entry point for the NodeSignal unit test suite.

Runs all test modules in order:
  test_packet_run      -- ns_packet_set boundary conditions (original suite)
  test_db_run          -- database user/message round-trip (original suite)
  test_comm_run        -- comm socket, wire encoding, protocol rejection
  test_db_extended_run -- extended database null guards, ordering, FK, limits
  test_integration_run -- end-to-end protocol exchange over loopback TCP
*/

#include "comm.h"

#include <stdio.h>

extern int test_packet_run(void);
extern int test_db_run(void);
extern int test_comm_run(void);
extern int test_db_extended_run(void);
extern int test_integration_run(void);

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
    if(test_comm_run() != 0) {
        exit_code = 1;
    }
    if(test_db_extended_run() != 0) {
        exit_code = 1;
    }
    if(test_integration_run() != 0) {
        exit_code = 1;
    }

    ns_net_cleanup();

    if(exit_code == 0) {
        puts("All tests passed.");
    }
    return exit_code;
}
