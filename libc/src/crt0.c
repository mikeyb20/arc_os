/* arc_os — C runtime startup
 * Provides _start entry point, calls main(argc, argv), then exit(). */

#include <stdint.h>
#include <stdlib.h>

extern int main(int argc, char **argv);

void _start(uint64_t argc, char **argv) {
    exit(main((int)argc, argv));
}
