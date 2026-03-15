/* arc_os — POSIX signal delivery */

#include "proc/signal.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "fs/vfs.h"
#include "lib/mem.h"
#include "user_access.h"

/* Global: RDI value to pass to signal handler (signo). Set by
 * sig_maybe_deliver, loaded by syscall_entry.asm before SYSRET. */
uint64_t sig_pending_arg;

/* Default actions: 0 = terminate, 1 = ignore */
static const uint8_t default_action[NSIG] = {
    [0]       = 0,
    [SIGHUP]  = 0,  /* terminate */
    [SIGINT]  = 0,  /* terminate */
    [SIGQUIT] = 0,  /* terminate */
    [SIGILL]  = 0,  /* terminate */
    [SIGABRT] = 0,  /* terminate */
    [SIGKILL] = 0,  /* terminate */
    [SIGSEGV] = 0,  /* terminate */
    [SIGPIPE] = 0,  /* terminate */
    [SIGALRM] = 0,  /* terminate */
    [SIGTERM] = 0,  /* terminate */
    [SIGCHLD] = 1,  /* ignore */
};

/* Sigreturn trampoline: machine code written onto user stack.
 *   mov eax, 22        ; SYS_SIGRETURN
 *   syscall
 *   ud2                ; should never reach here
 */
static const uint8_t sigreturn_trampoline[16] = {
    0xB8, 22, 0x00, 0x00, 0x00,   /* mov eax, 22 */
    0x0F, 0x05,                    /* syscall */
    0x0F, 0x0B,                    /* ud2 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* padding */
};

void sig_init(SigState *ss) {
    ss->pending = 0;
    ss->restoring = 0;
    for (int i = 0; i < NSIG; i++) {
        ss->handlers[i] = SIG_DFL;
    }
}

int sig_send(uint32_t pid, int signo) {
    if (signo < 1 || signo >= NSIG) return -EINVAL;

    Process *p = proc_get_by_pid(pid);
    if (p == NULL) return -ESRCH;

    p->sig.pending |= (1u << signo);
    return 0;
}

sig_handler_t sig_set_handler(SigState *ss, int signo, sig_handler_t handler) {
    if (signo < 1 || signo >= NSIG || signo == SIGKILL) return SIG_DFL;

    sig_handler_t old = ss->handlers[signo];
    ss->handlers[signo] = handler;
    return old;
}

/* Terminate a process due to a signal */
static void sig_terminate(Process *p, int signo) {
    p->exit_status = 128 + signo;
    p->state = PROC_ZOMBIE;
    p->main_thread->state = THREAD_DEAD;
    sched_schedule();
}

int64_t sig_maybe_deliver(SyscallFrame *frame, int64_t syscall_ret) {
    Process *p = proc_current();
    if (p == NULL) return syscall_ret;

    SigState *ss = &p->sig;

    /* 1. If sigreturn is in progress, restore the saved context */
    if (ss->restoring) {
        ss->restoring = 0;
        SignalFrame *sf = &ss->restore_frame;
        frame->rcx = sf->user_rip;       /* RIP for SYSRET */
        frame->r11 = sf->user_rflags;    /* RFLAGS for SYSRET */
        frame->rsp = sf->user_rsp;
        frame->rbx = sf->rbx;
        frame->rbp = sf->rbp;
        frame->r12 = sf->r12;
        frame->r13 = sf->r13;
        frame->r14 = sf->r14;
        frame->r15 = sf->r15;
        sig_pending_arg = 0;
        return (int64_t)sf->rax;  /* restore original RAX */
    }

    /* 2. No pending signals? Return immediately */
    if (ss->pending == 0) return syscall_ret;

    /* 3. Deliver one signal (lowest numbered first) */
    for (int signo = 1; signo < NSIG; signo++) {
        if (!(ss->pending & (1u << signo))) continue;

        /* Clear pending bit */
        ss->pending &= ~(1u << signo);

        /* SIGKILL always terminates, regardless of handler */
        if (signo == SIGKILL) {
            sig_terminate(p, signo);
            return syscall_ret;  /* unreachable after schedule */
        }

        sig_handler_t handler = ss->handlers[signo];

        if (handler == SIG_IGN) {
            continue;  /* ignored — try next signal */
        }

        if (handler == SIG_DFL) {
            if (default_action[signo] == 1) {
                continue;  /* default is ignore (e.g. SIGCHLD) */
            }
            /* Default is terminate */
            sig_terminate(p, signo);
            return syscall_ret;
        }

        /* User handler — build SignalFrame on user stack */
        uint64_t user_rsp = frame->rsp;
        user_rsp -= sizeof(SignalFrame);
        user_rsp &= ~0xFULL;  /* 16-byte align */

        if (!user_ptr_valid((void *)user_rsp, sizeof(SignalFrame))) {
            sig_terminate(p, signo);
            return syscall_ret;
        }

        SignalFrame *sf = (SignalFrame *)user_rsp;

        /* Write trampoline */
        memcpy(sf->trampoline, sigreturn_trampoline, 16);

        /* Save interrupted context */
        sf->user_rip    = frame->rcx;
        sf->user_rsp    = frame->rsp;
        sf->user_rflags = frame->r11;
        sf->rax         = (uint64_t)syscall_ret;
        sf->rbx         = frame->rbx;
        sf->rbp         = frame->rbp;
        sf->r12         = frame->r12;
        sf->r13         = frame->r13;
        sf->r14         = frame->r14;
        sf->r15         = frame->r15;
        sf->rdi         = 0;
        sf->rsi         = 0;
        sf->rdx         = 0;
        sf->r8          = 0;
        sf->r9          = 0;
        sf->r10         = 0;
        sf->signo       = (uint64_t)signo;
        sf->ret_addr    = user_rsp;  /* points to trampoline at start of frame */

        /* Redirect SYSRET to call handler(signo) */
        frame->rcx = (uint64_t)handler;  /* RIP = handler */
        frame->rsp = user_rsp;           /* RSP = signal frame */
        frame->rbx = 0;
        frame->rbp = 0;
        frame->r12 = 0;
        frame->r13 = 0;
        frame->r14 = 0;
        frame->r15 = 0;

        /* RDI = signo, loaded by asm stub */
        sig_pending_arg = (uint64_t)signo;

        return syscall_ret;  /* RAX value (handler ignores it) */
    }

    return syscall_ret;
}
