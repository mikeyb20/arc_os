#ifndef ARCHOS_PROC_SIGNAL_H
#define ARCHOS_PROC_SIGNAL_H

#include <stdint.h>

/* Signal numbers (POSIX subset) */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGABRT   6
#define SIGKILL   9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define NSIG     32

/* Signal handler type */
typedef void (*sig_handler_t)(int);

/* Special handler values */
#define SIG_DFL  ((sig_handler_t)0)
#define SIG_IGN  ((sig_handler_t)1)

/* User-stack signal frame — pushed before redirecting to handler.
 * Layout must match sigreturn expectations. */
typedef struct {
    uint8_t  trampoline[16]; /* sigreturn trampoline machine code */
    uint64_t user_rip;       /* interrupted RIP */
    uint64_t user_rsp;       /* interrupted RSP */
    uint64_t user_rflags;    /* interrupted RFLAGS */
    uint64_t rax;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t signo;
    uint64_t ret_addr;       /* points to trampoline */
} SignalFrame;

/* SyscallFrame — matches the push order in syscall_entry.asm.
 * Points into the kernel stack built by syscall_entry. */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rcx;   /* user RIP (from SYSCALL) */
    uint64_t r11;   /* user RFLAGS (from SYSCALL) */
    uint64_t rsp;   /* user RSP */
} SyscallFrame;

/* Per-process signal state */
typedef struct {
    uint32_t      pending;           /* Bitmask of pending signals */
    sig_handler_t handlers[NSIG];    /* Per-signal handlers */
    uint8_t       restoring;         /* 1 = sigreturn in progress */
    SignalFrame   restore_frame;     /* Saved frame for sigreturn */
} SigState;

/* Initialize signal state to defaults (all SIG_DFL, nothing pending). */
void sig_init(SigState *ss);

/* Send a signal to a process by PID. Returns 0 or -ESRCH/-EINVAL. */
int sig_send(uint32_t pid, int signo);

/* Set handler for signo in ss. Returns old handler. */
sig_handler_t sig_set_handler(SigState *ss, int signo, sig_handler_t handler);

/* Check and deliver pending signals after syscall_dispatch.
 * May modify the SyscallFrame to redirect to a signal handler.
 * Returns the value to place in RAX on return to user space. */
int64_t sig_maybe_deliver(SyscallFrame *frame, int64_t syscall_ret);

/* RDI value for signal handler — set by sig_maybe_deliver, loaded by asm. */
extern uint64_t sig_pending_arg;

#endif /* ARCHOS_PROC_SIGNAL_H */
