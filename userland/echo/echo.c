/* arc_os — Echo shell: reads lines from stdin, echoes them back
 * Proves the full keyboard → IRQ → TTY → sys_read → user process pipeline. */

#include <stdint.h>

/* Inline SYSCALL wrapper (3 arguments) */
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

void _start(void) {
    write_str("arc_os echo shell. Type something!\n");

    char buf[256];
    for (;;) {
        write_str("echo> ");

        /* SYS_READ(fd=0, buf, sizeof(buf)) */
        int64_t n = syscall3(4, 0, (uint64_t)buf, 256);
        if (n <= 0) break;

        /* Echo the line back */
        syscall3(1, 1, (uint64_t)buf, (uint64_t)n);
    }

    syscall3(0, 0, 0, 0);  /* SYS_EXIT */
    for (;;) __asm__ volatile ("ud2");
}
