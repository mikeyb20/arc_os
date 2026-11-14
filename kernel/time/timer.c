/* arc_os — Per-CPU one-shot timers (M1-06, ADR-0017)
 *
 * Owns: one timer queue per CPU (sleep timeouts, the scheduler's slice, the
 * block layer's request timeouts; later futex and poll timeouts) and the
 * programming of each CPU's clock-event hardware.
 *
 * Invariants:
 *  - A pending timer is in exactly one queue's tree (t->base), keyed by
 *    expires, equal expiries in arming order; base->first is the leftmost.
 *  - A CPU programs its own clock event only, always to base->first's expiry
 *    (or stops it when the queue is empty): no periodic tick, and an idle CPU
 *    with an empty queue gets no timer interrupt.
 *  - t->running counts t's callbacks executing now, on any CPU; it is raised
 *    in the same critical section that dequeues t, so timer_cancel_sync sees
 *    every timer either pending, running, or finished. It lives in the timer,
 *    not the queue: a timer re-armed onto another CPU while its callback still
 *    runs on the first must still be waited for.
 *
 * Locks: base->lock (LOCK_LEVEL_TIMER, IRQ-safe), one queue at a time. Taken
 * under the run-queue lock (the slice timer) and the block queue lock (request
 * timeouts); nothing but the log is taken under it, and callbacks run with
 * it dropped.
 *
 * Cost: arm and cancel O(log n) in the CPU's queued timers (red-black tree,
 * no allocation); finding the next expiry O(1) (cached leftmost); each
 * reprogram is one MSR or APIC register write. */

#include "time/timer.h"
#include "time/clock.h"
#include "proc/spinlock.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "mm/kmalloc.h"
#include "arch/hal.h"
#include "lib/kprintf.h"
#include "lib/panic.h"

/* Wait at most this long for every CPU to program its timer at boot */
#define TIMER_CPU_START_NS (2 * NSEC_PER_SEC)

typedef struct TimerBase {
    Spinlock lock;
    RbRoot tree;
    Timer *first;          /* earliest expiry, NULL when empty */
    uint64_t programmed;   /* expiry in the hardware, TIMER_NEVER when stopped */
    uint32_t cpu;
    uint32_t queued;
    uint64_t interrupts;
    uint64_t expired;
} TimerBase;

static TimerBase bsp_base;

static void base_init(TimerBase *b, uint32_t cpu) {
    spin_lock_init(&b->lock, LOCK_LEVEL_TIMER, "timer queue");
    b->tree.node = NULL;
    b->first = NULL;
    b->programmed = TIMER_NEVER;
    b->cpu = cpu;
}

static inline TimerBase *this_base(void) {
    return (TimerBase *)hal_this_cpu_timers();
}

/* --- Queue operations (b->lock held) --- */

static void base_insert(TimerBase *b, Timer *t) {
    RbNode **link = &b->tree.node, *parent = NULL;
    int leftmost = 1;
    while (*link != NULL) {
        parent = *link;
        if (t->expires < rb_entry(parent, Timer, node)->expires) {
            link = &parent->left;
        } else {
            link = &parent->right;   /* equal expiries: after the ones already there */
            leftmost = 0;
        }
    }
    rb_link_node(&t->node, parent, link);
    rb_insert_color(&t->node, &b->tree);
    if (leftmost) b->first = t;
    t->base = b;
    t->pending = 1;
    b->queued++;
}

static void base_remove(TimerBase *b, Timer *t) {
    if (b->first == t) {
        RbNode *next = rb_next(&t->node);
        b->first = next != NULL ? rb_entry(next, Timer, node) : NULL;
    }
    rb_erase(&t->node, &b->tree);
    t->pending = 0;
    b->queued--;
}

/* Point this CPU's clock event at the earliest expiry (b is this CPU's).
 * Skips the write when the hardware already holds it, unless force. */
static void base_program(TimerBase *b, int force) {
    uint64_t next = b->first != NULL ? b->first->expires : TIMER_NEVER;
    if (next == b->programmed && !force) return;
    __atomic_store_n(&b->programmed, next, __ATOMIC_RELAXED);   /* timer_next_event reads it */
    if (next == TIMER_NEVER)
        hal_clockevent_stop();
    else
        hal_clockevent_program(next);
}

/* --- Public operations --- */

void timer_init(Timer *t, TimerFn fn, void *arg) {
    t->node.parent = t->node.left = t->node.right = NULL;
    t->expires = TIMER_NEVER;
    t->fn = fn;
    t->arg = arg;
    t->base = NULL;
    t->pending = 0;
    t->running = 0;
}

/* Remove t from its queue if pending (that queue's lock taken here).
 * Reprograms only when that queue is this CPU's. */
static int timer_dequeue(Timer *t) {
    TimerBase *b = __atomic_load_n(&t->base, __ATOMIC_ACQUIRE);
    if (b == NULL) return 0;
    uint64_t flags;
    spin_lock_irqsave(&b->lock, &flags);
    int was = t->pending && t->base == b;
    if (was) {
        base_remove(b, t);
        if (b == this_base()) base_program(b, 0);
    }
    spin_unlock_irqrestore(&b->lock, flags);
    return was;
}

void timer_arm(Timer *t, uint64_t expires) {
    uint64_t flags = irq_save();   /* stay on this CPU while its queue is chosen */
    TimerBase *b = this_base();
    if (b == NULL) {
        kprintf("[TIMER] FATAL: timer armed on CPU %u before its queue exists\n", hal_cpu_index());
        KERNEL_PANIC();
    }
    if (t->pending && t->base != b) timer_dequeue(t);
    spin_lock(&b->lock);
    if (t->pending) base_remove(b, t);   /* re-arm on the same queue */
    if (expires != TIMER_NEVER) {
        t->expires = expires;
        base_insert(b, t);
    }
    base_program(b, 0);
    spin_unlock(&b->lock);
    irq_restore(flags);
}

int timer_cancel(Timer *t) {
    return timer_dequeue(t);
}

void timer_cancel_sync(Timer *t) {
    timer_dequeue(t);
    while (__atomic_load_n(&t->running, __ATOMIC_ACQUIRE) != 0) hal_cpu_relax();
}

int timer_cpu_ready(void) {
    return this_base() != NULL;
}

uint64_t timer_next_event(void) {
    TimerBase *b = this_base();
    return b != NULL ? __atomic_load_n(&b->programmed, __ATOMIC_RELAXED) : TIMER_NEVER;
}

int timer_pending(const Timer *t) {
    return __atomic_load_n(&t->pending, __ATOMIC_RELAXED);
}

/* --- The interrupt --- */

void timer_interrupt(void) {
    TimerBase *b = this_base();
    if (b == NULL) return;   /* not set up here (cannot be armed either) */
    spin_lock(&b->lock);     /* interrupts are off */
    b->interrupts++;
    uint64_t now = clock_mono_ns();
    Timer *t;
    while ((t = b->first) != NULL && t->expires <= now) {
        base_remove(b, t);
        __atomic_add_fetch(&t->running, 1, __ATOMIC_RELAXED);
        b->expired++;
        spin_unlock(&b->lock);
        t->fn(t->arg);
        spin_lock(&b->lock);
        __atomic_sub_fetch(&t->running, 1, __ATOMIC_RELEASE);   /* t is no longer touched */
        now = clock_mono_ns();
    }
    /* Always written: the one that fired is spent, and a thread may have
     * reprogrammed it while this interrupt was pending */
    base_program(b, 1);
    spin_unlock(&b->lock);
    sched_timer_exit();   /* a slice that ended switches threads now, outside the queue */
}

/* --- Sleeping --- */

static void sleep_expired(void *arg) {
    sched_wake((Thread *)arg);
}

int timer_sleep_until(uint64_t deadline) {
    if (clock_mono_ns() >= deadline) return 0;
    Thread *self = thread_current();
    Timer t;
    timer_init(&t, sleep_expired, self);
    uint64_t flags = irq_save();
    /* BLOCKED before the timer is armed: a wakeup between here and the switch
     * finds the thread still current and keeps it running (sched_wake) */
    self->state = THREAD_BLOCKED;
    timer_arm(&t, deadline);
    sched_schedule();
    irq_restore(flags);
    timer_cancel_sync(&t);   /* woken some other way, or the callback is still returning */
    return clock_mono_ns() >= deadline ? 0 : 1;
}

/* --- Bring-up and statistics --- */

static void clockevent_handler(void) {
    timer_interrupt();
}

/* Set up this CPU's clock-event hardware and program it for whatever is
 * already queued (IPI or BSP, interrupts off) */
static void timer_cpu_start(void *arg) {
    (void)arg;
    TimerBase *b = hal_this_cpu_timers();
    if (b == NULL) return;
    hal_clockevent_cpu_init();
    spin_lock(&b->lock);
    base_program(b, 1);
    spin_unlock(&b->lock);
}

void timer_init_boot(int one_shot) {
    if (!hal_clockevent_available()) {
        kprintf("[TIMER] FATAL: no local APIC: arc_os needs one for its timers\n");
        KERNEL_PANIC();
    }
    base_init(&bsp_base, 0);
    hal_cpu_set_timers(0, &bsp_base);
    hal_clockevent_init(clockevent_handler, one_shot);
    uint64_t flags = irq_save();
    timer_cpu_start(NULL);
    irq_restore(flags);
}

void timer_init_cpus(void) {
    uint32_t want = 0;
    for (uint32_t cpu = 1; cpu < HAL_MAX_CPUS; cpu++) {
        if (!hal_cpu_online(cpu)) continue;
        TimerBase *b = kmalloc(sizeof(TimerBase), GFP_ZERO);
        if (b == NULL) {
            kprintf("[TIMER] FATAL: out of memory for CPU %u's timer queue\n", cpu);
            KERNEL_PANIC();
        }
        base_init(b, cpu);
        hal_cpu_set_timers(cpu, b);
        want++;
    }
    if (want == 0) return;
    uint32_t done = hal_run_on_all_cpus(timer_cpu_start, NULL, hal_clock_ns_to_cycles(TIMER_CPU_START_NS));
    if (done != want) {
        kprintf("[TIMER] FATAL: %u of %u CPUs set up their timer\n", done, want);
        KERNEL_PANIC();
    }
}

int timer_cpu_stats(uint32_t cpu, TimerStats *out) {
    TimerBase *b = hal_cpu_timers(cpu);
    if (b == NULL) return -1;
    out->interrupts = __atomic_load_n(&b->interrupts, __ATOMIC_RELAXED);
    out->expired = __atomic_load_n(&b->expired, __ATOMIC_RELAXED);
    out->queued = __atomic_load_n(&b->queued, __ATOMIC_RELAXED);
    return 0;
}
