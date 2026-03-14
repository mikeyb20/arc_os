/* arc_os — Hello binary for exec() testing */

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

void _start(void) {
    const char *msg = "Hello from exec'd binary!\n";
    syscall3(1, 1, (uint64_t)msg, 26);
    syscall3(0, 0, 0, 0);
    for (;;) __asm__ volatile ("ud2");
}
