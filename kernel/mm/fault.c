/* arc_os — page-fault entry and the out-of-memory killer (mm.h, M1-07)
 *
 * Owns: the OOM policy. When a fault finds no free page, the process with
 * the largest resident set gets SIGKILL; if that is another process, its
 * anonymous memory is freed at once (as Linux's OOM reaper does) and the
 * fault is retried; if it is the faulting process, the fault ends and the
 * kill is delivered on the way back to user mode (or the user copy fails
 * with -EFAULT first). The kernel never panics for lack of user memory.
 * Invariants: a process with SIGKILL pending gets no new pages.
 * Locks: the victim is chosen under the kernel lock (the process list); the
 * victim's mm is pinned with a reference, because reaping takes its mm lock
 * and may sleep, which gives the kernel lock up.
 * Cost: a fault is one mm_fault; an OOM kill walks the process list once. */

#include "mm/mm.h"
#include "mm/pmm.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "proc/kernel_lock.h"
#include "lib/kprintf.h"

/* A fault that still finds no memory after this many kills kills itself */
#define OOM_MAX_KILLS 8

static int sigkill_pending(const Process *p) {
    return (__atomic_load_n(&p->sig.pending, __ATOMIC_RELAXED) & (1u << SIGKILL)) != 0;
}

typedef struct OomPick {
    Process *victim;
    uint64_t rss;
} OomPick;

static void oom_pick_cb(Process *p, void *ctx) {
    OomPick *o = ctx;
    /* init (PID 1) only if nothing else is left; dying ones are already going */
    if (p->mm == NULL || p->state == PROC_ZOMBIE || p->pid == 1 || sigkill_pending(p)) return;
    uint64_t rss = mm_rss(p->mm);
    if (o->victim == NULL || rss > o->rss) {
        o->victim = p;
        o->rss = rss;
    }
}

/* Kill for memory. Returns 1 if the fault should be retried, 0 if the
 * faulting process itself got SIGKILL. */
static int oom_kill(Process *self, int kill_self) {
    int held = kernel_lock_held();
    if (!held) kernel_lock();
    OomPick o = {NULL, 0};
    if (!kill_self) proc_foreach(oom_pick_cb, &o);
    Process *victim = o.victim != NULL && o.rss > 0 ? o.victim : self;
    uint32_t pid = victim->pid;
    kprintf("[OOM] out of memory (%lu pages free): killing pid %u, %lu KiB resident\n", pmm_get_free_pages(), pid,
            mm_rss(victim->mm) * (PAGE_SIZE / 1024));
    sig_send(pid, SIGKILL);
    int retry = 0;
    if (victim != self) {
        Mm *mm = victim->mm;
        mm_get(mm);   /* reaping may sleep: the victim may exit meanwhile */
        uint64_t freed = mm_reap(mm);
        mm_put(mm);
        kprintf("[OOM] reaped %lu pages of pid %u\n", freed, pid);
        retry = 1;
    }
    if (!held) kernel_unlock();
    return retry;
}

int mm_handle_fault(uint64_t addr, uint32_t access) {
    Process *p = proc_current();
    Mm *mm = p != NULL ? p->mm : NULL;
    if (mm == NULL) return MM_FAULT_MAPERR;
    for (int kills = 0;; kills++) {
        if (sigkill_pending(p)) return MM_FAULT_KILLED;
        int r = mm_fault(mm, addr, access);
        if (r != MM_FAULT_OOM) return r;
        if (!oom_kill(p, kills >= OOM_MAX_KILLS)) return MM_FAULT_KILLED;
    }
}
