#ifndef ARCHOS_PROC_SIGNAL_H
#define ARCHOS_PROC_SIGNAL_H

#include <stdint.h>

/* Signal numbers, Linux values (ADR-0001) */
#include "uapi/signal.h"

/* Signals 1..NSIG-1 are tracked; the pending set is one 32-bit word. */
#define NSIG     32

/* Signal handler type */
typedef void (*sig_handler_t)(int);

/* Special handler values */
#define SIG_DFL  ((sig_handler_t)0)
#define SIG_IGN  ((sig_handler_t)1)

/* Bit for signal n in a Linux sigset (blocked mask, sa_mask, uc_sigmask) */
#define SIGSET_BIT(n) (1ULL << ((n) - 1))

/* SyscallFrame — the user registers syscall_entry.asm saved, in its push
 * order (lowest address first). Signal delivery and rt_sigreturn rewrite it;
 * the stub reloads every field on the way out. RAX is not here: it is the
 * value sig_maybe_deliver returns. RCX and R11 hold the user RIP and RFLAGS
 * (SYSCALL put them there), so a signal frame records those for rcx/r11. */
typedef struct {
    uint64_t r9;
    uint64_t r8;
    uint64_t r10;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
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

/* Per-process signal state (one thread per process until M1-16/M1-17) */
typedef struct {
    uint32_t pending;            /* bit n = signal n pending */
    uint64_t blocked;            /* SIGSET_BIT(n) = signal n blocked */
    struct kernel_sigaction action[NSIG];       /* rt_sigaction: handler, flags, restorer, mask */
    uint32_t sender_pid[NSIG];   /* siginfo of the pending signal */
    uint32_t sender_uid[NSIG];
    uint8_t restoring;          /* rt_sigreturn: restore at syscall exit */
    uint64_t restore_frame;      /* user address of that rt_sigframe */
} SigState;

/* Initialize signal state to defaults (all SIG_DFL, nothing pending). */
void sig_init(SigState *ss);

/* Send a signal to a process by PID. Returns 0 or -ESRCH/-EINVAL. */
int sig_send(uint32_t pid, int signo);

/* rt_sigaction for one signal: copy the current action to old (if not NULL),
 * then install act (if not NULL). SIGKILL and SIGSTOP cannot be caught or
 * blocked (their sa_mask bits are dropped too). Returns 0, or -EINVAL for a
 * bad signal number or an act for SIGKILL/SIGSTOP. */
int sig_set_action(SigState *ss, int signo, const struct kernel_sigaction *act, struct kernel_sigaction *old);

/* Reset caught signals to SIG_DFL (execve): ignored ones stay ignored, the
 * blocked mask and pending signals stay (Linux). */
void sig_exec_reset(SigState *ss);

/* State a fork child starts with: the parent's actions and blocked mask, no
 * pending signals (Linux). */
void sig_fork_copy(SigState *child, const SigState *parent);

/* rt_sigreturn: remember the frame at user address frame_addr; the context is
 * restored when the syscall returns (sig_maybe_deliver). */
void sig_request_return(SigState *ss, uint64_t frame_addr);

/* Stack bytes one signal frame can take below the interrupted RSP, red zone
 * not counted (AT_MINSIGSTKSZ). */
uint64_t sig_frame_max_size(void);

/* Called at every syscall exit. First finishes an rt_sigreturn (restores the
 * registers, extended state and mask from the frame, or kills the process
 * with SIGSEGV if the frame is corrupt), then delivers the first pending
 * signal that is not blocked: default action, or a Linux rt_sigframe on the
 * user stack and a jump to the handler (RDI = signo, RSI = &info, RDX = &uc,
 * returning to sa_restorer). Returns the value for RAX. */
int64_t sig_maybe_deliver(SyscallFrame *frame, int64_t syscall_ret);

/* Send a signal to all processes in a process group. Returns 0 or -ESRCH. */
int sig_send_group(uint32_t pgid, int signo);

#endif /* ARCHOS_PROC_SIGNAL_H */
