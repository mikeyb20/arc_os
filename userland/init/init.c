/* arc_os — Init process (PID 1)
 * Execs the shell as the interactive user interface. */

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

void _start(uint64_t argc, char **argv) {
    (void)argc; (void)argv;
    /* Exec the shell — replaces this process image */
    const char *sh_argv[] = { "/boot/shell", (void *)0 };
    syscall3(17, (uint64_t)"/boot/shell", (uint64_t)sh_argv, 0);  /* SYS_EXEC */
    syscall3(0, 1, 0, 0);  /* exit(1) if exec fails */
    for (;;) __asm__ volatile ("ud2");
}
