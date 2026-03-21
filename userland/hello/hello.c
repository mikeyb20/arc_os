/* arc_os — Hello binary for exec() testing
 * Demonstrates argc/argv passing. */

#include <stdint.h>

static inline int64_t syscall3(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void write_str(const char *s) {
    uint64_t len = 0;
    while (s[len]) len++;
    syscall3(1, 1, (uint64_t)s, len);  /* SYS_WRITE(fd=1) */
}

void _start(uint64_t argc, char **argv) {
    if (argc <= 1) {
        write_str("Hello from exec'd binary!\n");
    } else {
        write_str("Hello");
        for (uint64_t i = 1; i < argc; i++) {
            write_str(" ");
            write_str(argv[i]);
        }
        write_str("!\n");
    }
    syscall3(0, 0, 0, 0);  /* SYS_EXIT */
    for (;;) __asm__ volatile ("ud2");
}
