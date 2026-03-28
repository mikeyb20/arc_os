#ifndef ARCHOS_LIBC_SYSCALL_H
#define ARCHOS_LIBC_SYSCALL_H

#include <stdint.h>

/* Syscall numbers — must match kernel/arch/x86_64/syscall.h */
#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_GETPID    2
#define SYS_OPEN      3
#define SYS_READ      4
#define SYS_CLOSE     5
#define SYS_BRK       6
#define SYS_LSEEK     7
#define SYS_STAT      8
#define SYS_MKDIR     9
#define SYS_READDIR   10
#define SYS_UNLINK    11
#define SYS_FSTAT     12
#define SYS_DUP       13
#define SYS_DUP2      14
#define SYS_GETPPID   15
#define SYS_FORK      16
#define SYS_EXEC      17
#define SYS_WAIT      18
#define SYS_PIPE      19
#define SYS_SIGNAL    20
#define SYS_KILL      21
#define SYS_SIGRETURN 22
#define SYS_CHDIR     23
#define SYS_GETCWD    24
#define SYS_GETUID    25
#define SYS_GETGID    26
#define SYS_SETUID    27
#define SYS_SETGID    28
#define SYS_CHMOD     29
#define SYS_CHOWN     30
#define SYS_SETPGID   31
#define SYS_GETPGID   32
#define SYS_TCSETPGRP 33
#define SYS_UMASK     34
#define SYS_SOCKET    35
#define SYS_BIND      36
#define SYS_LISTEN    37
#define SYS_ACCEPT    38
#define SYS_CONNECT   39
#define SYS_SEND      40
#define SYS_RECV      41
#define SYS_SENDTO    42
#define SYS_RECVFROM  43

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

#endif /* ARCHOS_LIBC_SYSCALL_H */
