/* arc_os — Minimal init process (PID 1)
 * Runs in user space (ring 3). Uses SYSCALL to communicate with kernel. */

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

void _start(void) {
    const char *msg = "Hello from user space!\n";
    /* SYS_WRITE(fd=1, buf, count=23) */
    syscall3(1, 1, (uint64_t)msg, 23);

    /* SYS_EXIT(status=0) */
    syscall3(0, 0, 0, 0);

    /* Should not reach here */
    for (;;) {
        __asm__ volatile ("ud2");
    }
}
