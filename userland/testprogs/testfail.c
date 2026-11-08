/* arc_os — Deliberately failing test program (M0-02 harness self-check).
 * Exits with status 3 so the harness must report "FAIL testfail exit=3". */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("testfail: failing on purpose\n");
    return 3;
}
