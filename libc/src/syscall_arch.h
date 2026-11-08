#ifndef ARCHOS_LIBC_SYSCALL_ARCH_H
#define ARCHOS_LIBC_SYSCALL_ARCH_H

#include <stdint.h>

/* Raw SYSCALL helpers, private to libarc: user programs call the libc
 * wrappers, never syscalls directly. Numbers are the Linux x86_64 __NR_*
 * values from the kernel's uapi header (ADR-0001). */
#include <uapi/unistd.h>

/* Turn a raw return into the libc convention: -1 with errno set on error. */
extern int errno;
static inline int64_t syscall_ret(int64_t ret) {
    if (ret < 0 && ret >= -4095) { errno = (int)(-ret); return -1; }
    return ret;
}

static inline int64_t syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t num, uint64_t a0) {
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall2(uint64_t num, uint64_t a0, uint64_t a1) {
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall4(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall5(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall6(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

#endif /* ARCHOS_LIBC_SYSCALL_ARCH_H */
