/* arc_os — POSIX signal delivery with Linux x86_64 signal frames (M1-01).
 *
 * Signals are delivered at syscall exit (sig_maybe_deliver, called by
 * syscall_entry.asm). A caught signal gets Linux's rt_sigframe on the user
 * stack: the XSAVE image (64-byte aligned), then the frame with ucontext and
 * siginfo; the handler returns to sa_restorer, which calls rt_sigreturn. */

#include "proc/signal.h"
#include <stddef.h>
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "fs/vfs.h"
#include "lib/mem.h"
#include "user_access.h"
#include "uapi/sigcontext.h"
#include "proc/proc_syscalls.h"
#include "arch/hal.h"

/* Default actions: 0 = terminate, 1 = ignore, 2 = continue, 3 = stop */
/* clang-format off */
static const uint8_t default_action[NSIG] = {
    [0]        = 0,
    [SIGHUP]   = 0,  /* terminate */
    [SIGINT]   = 0,  /* terminate */
    [SIGQUIT]  = 0,  /* terminate */
    [SIGILL]   = 0,  /* terminate */
    [SIGABRT]  = 0,  /* terminate */
    [SIGKILL]  = 0,  /* terminate */
    [SIGSEGV]  = 0,  /* terminate */
    [SIGPIPE]  = 0,  /* terminate */
    [SIGALRM]  = 0,  /* terminate */
    [SIGTERM]  = 0,  /* terminate */
    [SIGCHLD]  = 1,  /* ignore */
    [SIGURG]   = 1,  /* ignore */
    [SIGWINCH] = 1,  /* ignore */
    [SIGCONT]  = 2,  /* continue (special) */
    [SIGSTOP]  = 3,  /* stop (uncatchable) */
    [SIGTSTP]  = 3,  /* stop (catchable) */
    [SIGTTIN]  = 3,  /* stop */
    [SIGTTOU]  = 3,  /* stop */
};
/* clang-format on */

/* Neither can be caught, ignored or blocked */
#define SIG_UNBLOCKABLE (SIGSET_BIT(SIGKILL) | SIGSET_BIT(SIGSTOP))

/* Below the red zone, as Linux */
#define SIG_RED_ZONE 128

/* SYSRET faults in ring 0 on a non-canonical RIP: a handler address or a
 * restored RIP must lie below this (Linux's TASK_SIZE_MAX) */
#define SIG_USER_RIP_LIMIT 0x00007FFFFFFFF000ULL

/* RFLAGS bits rt_sigreturn takes from the frame, as Linux (minus RF, which
 * SYSRET clears anyway): the arithmetic flags, DF, TF and AC. A user #DB or
 * #AC raises a signal (M1-07), so user code may set TF and AC. */
#define SIG_FIX_EFLAGS 0x40DD5ULL   /* AC OF DF TF SF ZF AF PF CF */
#define RFLAGS_TF      (1ULL << 8)
#define RFLAGS_DF      (1ULL << 10)

/* User selectors SYSRET loads (sigcontext cs/ss) */
#define SIG_USER_CS (GDT_USER_CODE | 3)
#define SIG_USER_SS (GDT_USER_DATA | 3)

void sig_init(SigState *ss) {
    memset(ss, 0, sizeof(*ss));   /* all SIG_DFL, nothing pending or blocked */
}

/* Forward declarations */
static void sig_continue(Process *p);
static void sig_stop(Process *p, int signo);

/* Mark signo pending on p, recording the sender for siginfo (si_pid/si_uid
 * of a kill); SIGCONT resumes a stopped p at once. */
static void sig_post(Process *p, int signo) {
    Process *sender = proc_current();
    p->sig.sender_pid[signo] = sender ? sender->pid : 0;
    p->sig.sender_uid[signo] = sender ? sender->uid : 0;
    p->sig.pending |= (1u << signo);
    if (signo == SIGCONT && p->state == PROC_STOPPED) {
        sig_continue(p);
    }
}

int sig_send(uint32_t pid, int signo) {
    if (signo < 1 || signo >= NSIG) return -EINVAL;

    Process *p = proc_get_by_pid(pid);
    if (p == NULL) return -ESRCH;
    sig_post(p, signo);
    return 0;
}

int sig_set_action(SigState *ss, int signo, const struct kernel_sigaction *act, struct kernel_sigaction *old) {
    if (signo < 1 || signo >= NSIG) return -EINVAL;
    if (act != NULL && (signo == SIGKILL || signo == SIGSTOP)) return -EINVAL;
    struct kernel_sigaction next = act ? *act : ss->action[signo];   /* act may alias old */
    if (old != NULL) *old = ss->action[signo];
    if (act != NULL) {
        next.sa_mask &= ~SIG_UNBLOCKABLE;
        ss->action[signo] = next;
    }
    return 0;
}

void sig_exec_reset(SigState *ss) {
    /* Linux: caught signals go back to SIG_DFL; every action loses its flags,
     * restorer and mask (the old image's code is gone) */
    for (int i = 0; i < NSIG; i++) {
        uint64_t h = ss->action[i].sa_handler;
        memset(&ss->action[i], 0, sizeof(ss->action[i]));
        if (h == (uint64_t)SIG_IGN) ss->action[i].sa_handler = h;
    }
    ss->restoring = 0;
}

void sig_fork_copy(SigState *child, const SigState *parent) {
    sig_init(child);
    memcpy(child->action, parent->action, sizeof(child->action));
    child->blocked = parent->blocked;
}

void sig_request_return(SigState *ss, uint64_t frame_addr) {
    ss->restore_frame = frame_addr;
    ss->restoring = 1;
}

uint64_t sig_frame_max_size(void) {
    /* Linux's formula: frame + its 16-byte alignment, XSAVE image + magic2
     * + its 64-byte alignment, rounded up to 16 */
    uint64_t n = sizeof(struct rt_sigframe) + 15 + fpu_sigframe_size() + 63;
    return (n + 15) & ~15ULL;
}

static int sig_blocked(const SigState *ss, int signo) {
    return (ss->blocked & ~SIG_UNBLOCKABLE & SIGSET_BIT(signo)) != 0;
}

/* The signals in pending (bit n = signal n) that are not blocked. blocked is
 * a Linux sigset (bit n-1 = signal n), so it moves up one bit to compare. */
static uint32_t sig_deliverable(const SigState *ss, uint32_t pending) {
    return pending & ~(uint32_t)((ss->blocked & ~SIG_UNBLOCKABLE) << 1);
}

/* Stop a process due to a signal */
static void sig_stop(Process *p, int signo) {
    p->state = PROC_STOPPED;
    p->exit_status = signo;  /* Remember stop signal (non-zero = unreported) */
    p->main_thread->state = THREAD_STOPPED;   /* p is the current process: on no queue */
    /* Notify parent */
    if (p->parent != NULL) {
        wq_wake(&p->parent->child_exit_wq);
        sig_send(p->parent->pid, SIGCHLD);
    }
    sched_schedule();
}

/* Resume a stopped process */
static void sig_continue(Process *p) {
    if (p->state != PROC_STOPPED) return;
    p->state = PROC_ALIVE;
    sched_continue(p->main_thread);   /* safe if it is still switching away */
}

/* Terminate the current process p due to a signal: the exit path (proc_exit). */
static void sig_terminate(Process *p, int signo) {
    /* wait4 reports WIFSIGNALED from term_signal */
    proc_exit(p, 128 + signo, (uint8_t)signo);
}

/* Finish rt_sigreturn: registers, extended state and mask from the frame the
 * handler returned through. Returns 0, or -1 if the frame is unreadable or
 * would make SYSRET or XRSTOR fault (caller: SIGSEGV). */
static int sig_restore_frame(SigState *ss, SyscallFrame *frame, int64_t *rax) {
    ss->restoring = 0;
    struct ucontext uc;
    uint64_t uc_addr = ss->restore_frame + offsetof(struct rt_sigframe, uc);
    if (copy_from_user(&uc, (const void *)uc_addr, sizeof(uc)) != 0) return -1;
    const struct sigcontext *sc = &uc.uc_mcontext;
    if (sc->rip >= SIG_USER_RIP_LIMIT) return -1;
    if (fpu_signal_restore(thread_current(), sc->fpstate) != 0) return -1;

    frame->r8 = sc->r8;
    frame->r9 = sc->r9;
    frame->r10 = sc->r10;
    frame->r12 = sc->r12;
    frame->r13 = sc->r13;
    frame->r14 = sc->r14;
    frame->r15 = sc->r15;
    frame->rdi = sc->rdi;
    frame->rsi = sc->rsi;
    frame->rbp = sc->rbp;
    frame->rbx = sc->rbx;
    frame->rdx = sc->rdx;
    frame->rsp = sc->rsp;
    frame->rcx = sc->rip;   /* the RIP and RFLAGS slots */
    frame->r11 = (frame->r11 & ~SIG_FIX_EFLAGS) | (sc->eflags & SIG_FIX_EFLAGS);
    frame->ret_rcx = sc->rcx;   /* differ from RIP/RFLAGS: the stub leaves through IRET */
    frame->ret_r11 = sc->r11;
    *rax = (int64_t)sc->rax;
    ss->blocked = uc.uc_sigmask & ~SIG_UNBLOCKABLE;
    return 0;
}

/* Build the rt_sigframe for signo on the user stack and point the frame at
 * the handler. Returns 0, or -1 if the action has no restorer or the stack
 * can't take the frame (caller: SIGSEGV, as Linux). */
static int sig_setup_frame(Process *p, SyscallFrame *frame, int signo, int64_t syscall_ret) {
    SigState *ss = &p->sig;
    const struct kernel_sigaction *act = &ss->action[signo];
    if (!(act->sa_flags & SA_RESTORER)) return -1;   /* x86_64 Linux needs one */
    if (act->sa_handler >= SIG_USER_RIP_LIMIT) return -1;

    /* Placement as Linux: below the red zone the XSAVE image (64-byte
     * aligned), below it the frame, so that RSP + 8 is 16-byte aligned at
     * handler entry (as after a call) */
    uint64_t need = SIG_RED_ZONE + sig_frame_max_size();
    if (frame->rsp < need || frame->rsp > USER_ADDR_LIMIT) return -1;
    uint64_t fp_addr = (frame->rsp - SIG_RED_ZONE - fpu_sigframe_size()) & ~63ULL;
    uint64_t sf_addr = ((fp_addr - sizeof(struct rt_sigframe)) & ~15ULL) - 8;

    if (fpu_signal_save(thread_current(), fp_addr) != 0) return -1;

    struct rt_sigframe sf;
    memset(&sf, 0, sizeof(sf));
    sf.pretcode = act->sa_restorer;
    sf.uc.uc_flags = UC_FP_XSTATE | UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS;
    sf.uc.uc_stack.ss_flags = SS_DISABLE;   /* no sigaltstack yet (M1-17) */
    struct sigcontext *sc = &sf.uc.uc_mcontext;
    sc->r8 = frame->r8;
    sc->r9 = frame->r9;
    sc->r10 = frame->r10;
    sc->r11 = frame->ret_r11;
    sc->r12 = frame->r12;
    sc->r13 = frame->r13;
    sc->r14 = frame->r14;
    sc->r15 = frame->r15;
    sc->rdi = frame->rdi;
    sc->rsi = frame->rsi;
    sc->rbp = frame->rbp;
    sc->rbx = frame->rbx;
    sc->rdx = frame->rdx;
    sc->rax = (uint64_t)syscall_ret;
    sc->rcx = frame->ret_rcx;
    sc->rsp = frame->rsp;
    sc->rip = frame->rcx;
    sc->eflags = frame->r11;
    sc->cs = SIG_USER_CS;
    sc->ss = SIG_USER_SS;
    sc->oldmask = ss->blocked;
    sc->fpstate = fp_addr;
    sf.uc.uc_sigmask = ss->blocked;
    sf.info.si_signo = signo;
    if (signo == ss->fault_signo) {
        sf.info.si_code = ss->fault_code;
        if (ss->fault_code != SI_KERNEL) sf.info.si_addr = ss->fault_addr;
        ss->fault_signo = 0;
    } else {
        sf.info.si_code = SI_USER;
        sf.info.si_pid = ss->sender_pid[signo];
        sf.info.si_uid = ss->sender_uid[signo];
    }
    if (copy_to_user((void *)sf_addr, &sf, sizeof(sf)) != 0) return -1;

    /* The handler runs with sa_mask (and signo itself, unless SA_NODEFER)
     * blocked, and with a clean FPU state */
    ss->blocked |= act->sa_mask;
    if (!(act->sa_flags & SA_NODEFER)) ss->blocked |= SIGSET_BIT(signo);
    ss->blocked &= ~SIG_UNBLOCKABLE;
    uint64_t handler = act->sa_handler;
    if (act->sa_flags & SA_RESETHAND) memset(&ss->action[signo], 0, sizeof(ss->action[signo]));
    fpu_load_init();

    frame->rcx = handler;
    frame->rsp = sf_addr;
    frame->rdi = (uint64_t)signo;
    frame->rsi = sf_addr + offsetof(struct rt_sigframe, info);
    frame->rdx = sf_addr + offsetof(struct rt_sigframe, uc);
    frame->r11 &= ~(RFLAGS_DF | RFLAGS_TF);
    frame->ret_rcx = frame->rcx;   /* handler entry: SYSRET is fine */
    frame->ret_r11 = frame->r11;
    return 0;
}

/* The next signal to deliver: a fault's first, then the lowest pending one
 * that is not blocked; 0 if none */
static int sig_next(const SigState *ss) {
    uint32_t ready = sig_deliverable(ss, ss->pending) & ~1u;   /* bit 0 is no signal */
    int f = ss->fault_signo;
    if (f != 0 && (ready & (1u << f))) return f;
    return ready != 0 ? __builtin_ctz(ready) : 0;
}

void sig_force_fault(int signo, int code, uint64_t addr) {
    Process *p = proc_current();
    if (p == NULL || signo < 1 || signo >= NSIG) return;
    SigState *ss = &p->sig;
    if (sig_blocked(ss, signo) || ss->action[signo].sa_handler == (uint64_t)SIG_IGN) {
        memset(&ss->action[signo], 0, sizeof(ss->action[signo]));   /* SIG_DFL */
        ss->blocked &= ~SIGSET_BIT(signo);
    }
    sig_post(p, signo);
    ss->fault_signo = (uint8_t)signo;
    ss->fault_code = code;
    ss->fault_addr = addr;
}

int sig_work_pending(void) {
    Process *p = proc_current();
    if (p == NULL) return 0;
    uint32_t pending = __atomic_load_n(&p->sig.pending, __ATOMIC_RELAXED);
    return (sig_deliverable(&p->sig, pending) & ~1u) != 0;
}

void sig_fatal(int signo) {
    sig_terminate(proc_current(), signo);
    __builtin_unreachable();
}

int64_t sig_maybe_deliver(SyscallFrame *frame, int64_t syscall_ret) {
    Process *p = proc_current();
    if (p == NULL) return syscall_ret;

    SigState *ss = &p->sig;

    if (ss->restoring && sig_restore_frame(ss, frame, &syscall_ret) != 0) {
        sig_terminate(p, SIGSEGV);
        return syscall_ret;
    }
    int signo;
    while ((signo = sig_next(ss)) != 0) {
        ss->pending &= ~(1u << signo);

        if (signo == SIGKILL) {
            sig_terminate(p, signo);
            return syscall_ret;
        }
        if (signo == SIGSTOP) {
            sig_stop(p, signo);
            return syscall_ret;
        }

        sig_handler_t handler = (sig_handler_t)ss->action[signo].sa_handler;
        if (handler == SIG_IGN) continue;
        if (handler == SIG_DFL) {
            uint8_t action = default_action[signo];
            if (action == 1) continue;           /* ignore */
            if (action == 2) continue;           /* continue: no-op here */
            if (action == 3) {                   /* stop */
                sig_stop(p, signo);
                return syscall_ret;
            }
            sig_terminate(p, signo);
            return syscall_ret;
        }

        if (sig_setup_frame(p, frame, signo, syscall_ret) < 0) {
            sig_terminate(p, SIGSEGV);
            return syscall_ret;
        }
        return 0;   /* RAX = 0 at handler entry, as Linux */
    }

    return syscall_ret;
}

/* --- Process group signal delivery --- */

typedef struct {
    pid_t pgid;
    int   signo;
    int   count;
} SigGroupCtx;

static void sig_group_cb(Process *p, void *ctx) {
    SigGroupCtx *sg = (SigGroupCtx *)ctx;
    if (p->pgid == sg->pgid) {
        sig_post(p, sg->signo);
        sg->count++;
    }
}

int sig_send_group(uint32_t pgid, int signo) {
    if (signo < 1 || signo >= NSIG) return -EINVAL;

    SigGroupCtx ctx = { .pgid = pgid, .signo = signo, .count = 0 };
    proc_foreach(sig_group_cb, &ctx);

    return ctx.count > 0 ? 0 : -ESRCH;
}
