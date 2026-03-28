/* arc_os — C runtime startup
 * Provides _start entry point, calls main(argc, argv), then exit(). */

#include <syscall.h>

extern int main(int argc, char **argv);

void _start(uint64_t argc, char **argv) {
    int ret = main((int)argc, argv);
    syscall1(SYS_EXIT, (uint64_t)ret);
    for (;;) __asm__ volatile ("ud2");
}
