#ifndef ARCHOS_UAPI_SIGNAL_H
#define ARCHOS_UAPI_SIGNAL_H

#include <stdint.h>

/* Linux x86_64 signal numbers, rt_sigaction layout and flags (ADR-0001). */

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

/* The kernel's sigset_t is one 64-bit word; rt_sigaction and rt_sigprocmask
 * take this size as their last argument and reject anything else. */
#define ARC_SIGSET_SIZE 8

/* sa_flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

/* rt_sigprocmask how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* The kernel's struct sigaction (not libc's): what rt_sigaction reads and writes. */
struct kernel_sigaction {
    uint64_t sa_handler;       /* handler address, or 0 = SIG_DFL, 1 = SIG_IGN */
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;          /* bit (n - 1) = signal n */
};

_Static_assert(sizeof(struct kernel_sigaction) == 32, "kernel sigaction must be 32 bytes");
_Static_assert(__builtin_offsetof(struct kernel_sigaction, sa_flags) == 8, "sa_flags offset");
_Static_assert(__builtin_offsetof(struct kernel_sigaction, sa_restorer) == 16, "sa_restorer offset");
_Static_assert(__builtin_offsetof(struct kernel_sigaction, sa_mask) == 24, "sa_mask offset");

#endif /* ARCHOS_UAPI_SIGNAL_H */
