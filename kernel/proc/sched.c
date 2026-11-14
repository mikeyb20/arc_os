/* arc_os — SMP scheduler (M1-05)
 *
 * Owns: one run queue per CPU (FIFO round robin with SCHED_SLICE_NS slices),
 * each CPU's idle thread, the CPU a woken thread runs on, idle-pull
 * balancing, affinity masks and the per-CPU statistics.
 *
 * Invariants:
 *  - A READY thread is on exactly one run queue (on_rq, cpu = that queue's
 *    CPU), or in flight between a wakeup's claim and its enqueue. A RUNNING
 *    thread is rq->curr of one CPU and on no queue. BLOCKED, STOPPED and DEAD
 *    threads are on no queue.
 *  - A CPU switches threads only while holding its own run-queue lock. The
 *    lock is carried across context_switch and released by the thread switched
 *    to (sched_finish_switch), so whoever holds a queue's lock sees each of
 *    its threads either as rq->curr or fully switched out: a queued thread is
 *    never still running.
 *  - on_cpu drops to 0 only after the thread's switch away has finished and
 *    its stack is out of use (proc_reap waits for that before freeing it).
 *  - No CPU changes a thread another CPU is running: a wakeup that finds its
 *    thread not yet switched away just sets it RUNNING again, and an affinity
 *    change moves a running thread at its next schedule.
 *  - The idle thread never sits on a queue; it runs when the queue is empty.
 *
 * Locks: rq->lock (LOCK_LEVEL_RUNQUEUE), always with interrupts off; two at
 * once only in CPU-index order (subkey = index + 1). Wakeups take it under
 * wait-queue locks. A thread holding the kernel lock gives it up across its
 * switch (kernel_lock_suspend) and takes it back after.
 *
 * Cost: picking the next thread is O(1) from the local queue; a wakeup looks
 * at each online CPU once (O(CPUs)); an idle CPU's pull looks at each online
 * CPU once and at no more than SCHED_PULL_SCAN threads of the busiest queue.
 * No global lock in schedule; a switch does no lookup by ID (a thread points
 * to its process). Time accounting reads the TSC once per switch. */

#include "proc/sched.h"
#include "proc/process.h"
#include "proc/spinlock.h"
#include "proc/kernel_lock.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "mm/mm.h"
#include "arch/hal.h"
#include "lib/kprintf.h"

_Static_assert(SCHED_MAX_CPUS == HAL_MAX_CPUS, "affinity masks cover every CPU index");

/* Threads of the busiest queue an idle CPU looks at for one it may run */
#define SCHED_PULL_SCAN 8

typedef struct RunQueue {
    Spinlock lock;
    Thread *head;               /* READY threads, FIFO through rq_next/rq_prev */
    Thread *tail;
    uint32_t nr_queued;
    uint32_t cpu;
    Thread *curr;               /* running here; NULL until this CPU schedules */
    Thread *idle;
    Thread *prev;               /* switched away from, until sched_finish_switch */
    Thread *migrate;            /* preempted thread its affinity sends elsewhere */
    uint64_t last_switch_tsc;
    uint64_t idle_cycles;
    uint64_t busy_cycles;
    uint64_t nr_switches;
    uint64_t nr_migrations;
    int core;                   /* HAL_CORE_* */
    LockdepCpu lockdep;
} RunQueue;

_Static_assert(sizeof(RunQueue) <= 320, "run queue: a few cache lines per CPU");

static RunQueue bsp_rq __attribute__((aligned(64)));
static CpuMask online_mask;          /* CPUs whose run queue takes threads */
static uint32_t sched_cpu_limit;     /* highest such CPU index + 1 */
static CpuMask core_masks[3];        /* by HAL_CORE_* */
static int slice_timer_ready;

static inline RunQueue *this_rq(void) {
    return (RunQueue *)hal_this_cpu_sched();
}

static inline RunQueue *cpu_rq(uint32_t cpu) {
    return (RunQueue *)hal_cpu_sched(cpu);
}

static void rq_init(RunQueue *rq, uint32_t cpu) {
    spin_lock_init(&rq->lock, LOCK_LEVEL_RUNQUEUE, "run queue");
    rq->lock.subkey = (uint16_t)(cpu + 1);
    rq->cpu = cpu;
    rq->core = HAL_CORE_OTHER;
}

/* Make rq take threads (after it is fully set up). */
static void rq_online(RunQueue *rq) {
    int core = hal_cpu_core_kind(rq->cpu);
    rq->core = core < 0 ? HAL_CORE_OTHER : core;
    cpumask_set(&core_masks[rq->core], rq->cpu);
    hal_cpu_set_sched(rq->cpu, rq);
    if (rq->cpu + 1 > sched_cpu_limit) sched_cpu_limit = rq->cpu + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    cpumask_set(&online_mask, rq->cpu);
}

/* --- Queue operations (rq->lock held) --- */

static void rq_enqueue(RunQueue *rq, Thread *t) {
    t->rq_next = NULL;
    t->rq_prev = rq->tail;
    if (rq->tail != NULL)
        rq->tail->rq_next = t;
    else
        rq->head = t;
    rq->tail = t;
    t->on_rq = 1;
    t->cpu = rq->cpu;
    t->state = THREAD_READY;
    rq->nr_queued++;
}

static void rq_dequeue(RunQueue *rq, Thread *t) {
    if (t->rq_prev != NULL)
        t->rq_prev->rq_next = t->rq_next;
    else
        rq->head = t->rq_next;
    if (t->rq_next != NULL)
        t->rq_next->rq_prev = t->rq_prev;
    else
        rq->tail = t->rq_prev;
    t->rq_next = t->rq_prev = NULL;
    t->on_rq = 0;
    rq->nr_queued--;
}

/* Lock two run queues in CPU-index order (interrupts off). */
static void rq_lock_two(RunQueue *a, RunQueue *b) {
    if (a->cpu < b->cpu) {
        spin_lock(&a->lock);
        spin_lock(&b->lock);
    } else {
        spin_lock(&b->lock);
        spin_lock(&a->lock);
    }
}

/* --- Choosing a CPU --- */

/* Threads queued or running there (the idle thread doesn't count). Read
 * without the lock: a hint. */
static uint32_t rq_load(const RunQueue *rq) {
    Thread *curr = __atomic_load_n(&rq->curr, __ATOMIC_RELAXED);
    return __atomic_load_n(&rq->nr_queued, __ATOMIC_RELAXED) + (curr != NULL && curr != rq->idle);
}

/* The CPU t should run on: the one it last ran on if allowed and idle, else
 * the least loaded allowed CPU (ties go to the last one, then the lowest). */
static uint32_t sched_select_cpu(const Thread *t) {
    uint32_t last = t->cpu;
    if (cpumask_test(&online_mask, last) && cpumask_test(&t->affinity, last) && rq_load(cpu_rq(last)) == 0) {
        return last;
    }
    uint32_t best = UINT32_MAX;
    uint32_t best_load = UINT32_MAX;
    for (uint32_t c = 0; c < sched_cpu_limit; c++) {
        if (!cpumask_test(&online_mask, c) || !cpumask_test(&t->affinity, c)) continue;
        uint32_t load = rq_load(cpu_rq(c));
        if (load < best_load || (load == best_load && c == last)) {
            best = c;
            best_load = load;
        }
    }
    return best != UINT32_MAX ? best : 0;   /* affinity always meets online_mask */
}

/* Queue t (READY, on no queue, not running) on cpu; wake that CPU if idle. */
static void sched_enqueue_on(uint32_t cpu, Thread *t) {
    RunQueue *rq = cpu_rq(cpu);
    uint64_t flags;
    spin_lock_irqsave(&rq->lock, &flags);
    rq_enqueue(rq, t);
    int kick = rq->curr != NULL && rq->curr == rq->idle && cpu != hal_cpu_index();
    spin_unlock_irqrestore(&rq->lock, flags);
    /* An idle CPU otherwise sleeps until its next interrupt. A busy one picks
     * the thread up at its slice end; an idle self is in an interrupt handler
     * and its idle loop looks again on the way out. */
    if (kick) hal_send_resched(cpu);
}

void sched_add_thread(Thread *t) {
    t->state = THREAD_READY;
    sched_enqueue_on(sched_select_cpu(t), t);
}

/* Wake t if it is in state from. */
static int sched_wake_from(Thread *t, uint8_t from) {
    for (;;) {
        uint32_t cpu = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);
        RunQueue *rq = cpu_rq(cpu);
        uint64_t flags;
        spin_lock_irqsave(&rq->lock, &flags);
        if (t->cpu != cpu) {   /* moved meanwhile: lock the right queue */
            spin_unlock_irqrestore(&rq->lock, flags);
            continue;
        }
        if (t->state != from) {
            spin_unlock_irqrestore(&rq->lock, flags);
            return 0;
        }
        if (rq->curr == t) {
            /* Still on its CPU on the way to sleep: it just keeps running */
            t->state = THREAD_RUNNING;
            spin_unlock_irqrestore(&rq->lock, flags);
            return 1;
        }
        t->state = THREAD_READY;   /* claimed: a second waker sees READY and stops */
        spin_unlock_irqrestore(&rq->lock, flags);
        sched_enqueue_on(sched_select_cpu(t), t);
        return 1;
    }
}

int sched_wake(Thread *t) {
    return sched_wake_from(t, THREAD_BLOCKED);
}

int sched_continue(Thread *t) {
    return sched_wake_from(t, THREAD_STOPPED);
}

/* --- Switching --- */

static Mm *thread_mm(const Thread *t) {
    return t->proc != NULL ? t->proc->mm : NULL;
}

/* Account the time since the last switch to the thread that had the CPU. */
static void rq_account(RunQueue *rq, const Thread *prev) {
    uint64_t now = hal_tsc_read();
    uint64_t delta = now - rq->last_switch_tsc;
    rq->last_switch_tsc = now;
    if (prev == rq->idle)
        rq->idle_cycles += delta;
    else
        rq->busy_cycles += delta;
}

void sched_schedule(void) {
    RunQueue *rq = this_rq();
    Thread *prev = rq->curr;
    /* No spinlock across a switch, blocking or not: the next thread here
     * could spin on it forever with interrupts off */
    lockdep_assert_can_sleep(prev->state != THREAD_RUNNING ? "blocking in sched_schedule"
                                                           : "yielding in sched_schedule");
    int had_kernel_lock = kernel_lock_suspend();

    spin_lock(&rq->lock);   /* interrupts are off (caller) */
    if (prev->state == THREAD_RUNNING && prev != rq->idle) {
        if (cpumask_test(&prev->affinity, rq->cpu)) {
            rq_enqueue(rq, prev);
        } else {
            prev->state = THREAD_READY;   /* re-queued elsewhere once switched out */
            rq->migrate = prev;
        }
    }

    Thread *next = rq->head;
    if (next != NULL) {
        rq_dequeue(rq, next);
    } else if (prev->state == THREAD_RUNNING) {
        next = prev;   /* the idle thread with nothing to do */
    } else {
        next = rq->idle;
    }
    if (next == NULL) {
        kprintf("[SCHED] FATAL: CPU %u has nothing to run\n", rq->cpu);
        KERNEL_PANIC();
    }
    next->state = THREAD_RUNNING;

    if (next == prev) {
        spin_unlock(&rq->lock);
        kernel_lock_resume(had_kernel_lock);
        return;
    }

    rq_account(rq, prev);
    rq->nr_switches++;
    rq->prev = prev;
    __atomic_store_n(&rq->curr, next, __ATOMIC_RELAXED);
    next->on_cpu = 1;
    thread_set_current(next);

    /* This CPU's entry stacks (TSS.RSP0 and SYSCALL) for the new thread */
    if (next->kernel_stack_top != 0) hal_set_kernel_stack(next->kernel_stack_top);

    /* Address space: mm_switch compares with what this CPU has loaded (its
     * own record, which every CR3 load goes through) and keeps the mm's
     * loaded-on set right for TLB shootdowns (M1-07) */
    mm_switch(thread_mm(next));

    /* Slices only while a real thread runs: idle CPUs get no timer interrupts */
    if (slice_timer_ready) {
        if (next == rq->idle)
            hal_slice_timer_stop();
        else
            hal_slice_timer_start(SCHED_SLICE_NS);
    }

    /* User FPU/SSE state and FS base (TLS), eagerly (fpu.h) */
    arch_thread_switch(prev, next);
    context_switch(&prev->context, &next->context);

    /* prev runs again here, maybe on another CPU */
    sched_finish_switch();
    kernel_lock_resume(had_kernel_lock);
}

void sched_finish_switch(void) {
    RunQueue *rq = this_rq();
    Thread *prev = rq->prev;
    Thread *migrating = rq->migrate;
    rq->prev = NULL;
    rq->migrate = NULL;
    /* prev's registers are saved and this CPU is off its stack */
    if (prev != NULL) __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);
    spin_unlock(&rq->lock);
    if (migrating != NULL) sched_enqueue_on(sched_select_cpu(migrating), migrating);
}

int sched_can_block(void) {
    RunQueue *rq = this_rq();
    return rq != NULL && rq->idle != NULL;
}

void sched_yield(void) {
    uint64_t flags = irq_save();
    sched_schedule();
    irq_restore(flags);
}

void sched_preempt(void) {
    RunQueue *rq = this_rq();
    if (rq == NULL || rq->curr == NULL) return;   /* this AP isn't scheduling yet */
    if (hal_lock_depth() != 0) return;            /* never preempt a spinlock holder */
    sched_schedule();
}

void sched_wait_off_cpu(Thread *t) {
    while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) hal_cpu_relax();
}

/* --- Idle and balancing --- */

/* An idle CPU takes one queued thread it may run from the busiest queue. */
static void sched_pull(RunQueue *rq) {
    RunQueue *busiest = NULL;
    uint32_t most = 0;
    for (uint32_t c = 0; c < sched_cpu_limit; c++) {
        if (c == rq->cpu || !cpumask_test(&online_mask, c)) continue;
        RunQueue *other = cpu_rq(c);
        uint32_t n = __atomic_load_n(&other->nr_queued, __ATOMIC_RELAXED);
        if (n > most) {
            most = n;
            busiest = other;
        }
    }
    if (busiest == NULL) return;

    rq_lock_two(rq, busiest);
    Thread *t = busiest->tail;
    for (int i = 0; t != NULL && i < SCHED_PULL_SCAN; i++, t = t->rq_prev) {
        if (cpumask_test(&t->affinity, rq->cpu)) break;
    }
    if (t != NULL && cpumask_test(&t->affinity, rq->cpu)) {
        rq_dequeue(busiest, t);
        rq_enqueue(rq, t);
        rq->nr_migrations++;
    }
    spin_unlock(&busiest->lock);
    spin_unlock(&rq->lock);
}

/* The idle thread's loop: run queued work, else pull some, else halt until an
 * interrupt (a wakeup IPI, or a device). No polling, no timer (ADR-0017). */
__attribute__((noreturn)) static void sched_idle_loop(void) {
    for (;;) {
        (void)irq_save();   /* off: no wakeup slips in between the check and the halt */
        RunQueue *rq = this_rq();
        if (__atomic_load_n(&rq->nr_queued, __ATOMIC_RELAXED) == 0) sched_pull(rq);
        if (__atomic_load_n(&rq->nr_queued, __ATOMIC_RELAXED) != 0) {
            sched_schedule();
            continue;
        }
        hal_idle_wait();    /* interrupts on and halt, atomically */
    }
}

static void sched_idle_entry(void *arg) {
    (void)arg;
    sched_idle_loop();
}

/* --- Bring-up --- */

void sched_init(void) {
    RunQueue *rq = &bsp_rq;
    rq_init(rq, 0);
    Thread *boot = thread_current();
    boot->cpu = 0;
    boot->on_cpu = 1;
    rq->curr = boot;
    rq->last_switch_tsc = hal_tsc_read();
    rq_online(rq);
    kprintf("[SCHED] Per-CPU run queues, round robin, %lu ms slices\n", (uint64_t)(SCHED_SLICE_NS / 1000000));
}

void sched_start_aps(void) {
    static int started;
    if (started) return;
    started = 1;

    /* The boot thread runs on the bootloader's stack of CPU 0: keep it there */
    Thread *boot = thread_current();
    cpumask_clear(&boot->affinity);
    cpumask_set(&boot->affinity, 0);

    uint32_t online = 1;
    for (uint32_t cpu = 1; cpu < SCHED_MAX_CPUS; cpu++) {
        if (!hal_cpu_online(cpu)) continue;
        RunQueue *rq = kmalloc(sizeof(RunQueue), GFP_ZERO);
        Thread *idle = rq != NULL ? thread_create(sched_idle_entry, NULL) : NULL;
        if (idle == NULL) {
            kprintf("[SCHED] WARNING: out of memory for CPU %u's run queue; it stays idle\n", cpu);
            kfree(rq);
            continue;
        }
        rq_init(rq, cpu);
        rq->idle = idle;
        idle->cpu = cpu;
        cpumask_clear(&idle->affinity);
        cpumask_set(&idle->affinity, cpu);
        rq_online(rq);
        online++;
    }

    hal_slice_timer_init(sched_preempt);
    slice_timer_ready = 1;
    kprintf("[SCHED] %u CPUs scheduling (P-cores, E-cores split where the CPU is hybrid)\n", online);
    hal_release_aps();
}

void sched_start(void) {
    this_rq()->idle = thread_current();   /* the boot thread idles CPU 0 from here */
    sched_start_aps();
    sched_idle_loop();
}

void sched_ap_start(void) {
    RunQueue *rq = this_rq();
    if (rq == NULL) {   /* no run queue (out of memory at boot): stay out */
        for (;;) hal_halt_forever();
    }
    Thread *idle = rq->idle;
    hal_switch_address_space(vmm_get_kernel_pml4());   /* off the bootloader's page tables */

    spin_lock(&rq->lock);   /* interrupts are off; the idle thread releases it */
    rq->last_switch_tsc = hal_tsc_read();
    idle->state = THREAD_RUNNING;
    idle->on_cpu = 1;
    __atomic_store_n(&rq->curr, idle, __ATOMIC_RELAXED);
    thread_set_current(idle);
    hal_set_kernel_stack(idle->kernel_stack_top);

    ThreadContext boot_context;   /* the bootloader's stack: never resumed */
    context_switch(&boot_context, &idle->context);
    for (;;) hal_halt_forever();
}

/* --- Affinity --- */

int sched_set_affinity(Thread *t, const CpuMask *mask) {
    if (!cpumask_intersects(mask, &online_mask)) return -1;
    for (;;) {
        uint32_t cpu = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);
        RunQueue *rq = cpu_rq(cpu);
        uint64_t flags;
        spin_lock_irqsave(&rq->lock, &flags);
        if (t->cpu != cpu) {
            spin_unlock_irqrestore(&rq->lock, flags);
            continue;
        }
        t->affinity = *mask;
        int move = 0, kick = 0;
        if (!cpumask_test(mask, cpu)) {
            if (t->on_rq) {
                rq_dequeue(rq, t);
                t->state = THREAD_READY;
                move = 1;
            } else if (rq->curr == t) {
                kick = 1;   /* running there: it moves itself at its next schedule */
            }
        }
        spin_unlock_irqrestore(&rq->lock, flags);
        if (move) sched_enqueue_on(sched_select_cpu(t), t);
        if (kick) {
            if (t == thread_current())
                sched_yield();
            else
                hal_send_resched(cpu);
        }
        return 0;
    }
}

void sched_core_mask(int core, CpuMask *out) {
    if (core == SCHED_CORE_P)
        *out = core_masks[HAL_CORE_P];
    else if (core == SCHED_CORE_E)
        *out = core_masks[HAL_CORE_E];
    else
        cpumask_clear(out);
}

/* --- Statistics and the lock checker --- */

LockdepCpu *sched_this_lockdep(void) {
    RunQueue *rq = this_rq();
    return rq != NULL ? &rq->lockdep : NULL;
}

int sched_cpu_stats(uint32_t cpu, SchedCpuStats *out) {
    if (!cpumask_test(&online_mask, cpu)) return -1;
    RunQueue *rq = cpu_rq(cpu);
    out->nr_running = rq_load(rq);
    out->nr_switches = __atomic_load_n(&rq->nr_switches, __ATOMIC_RELAXED);
    out->nr_migrations = __atomic_load_n(&rq->nr_migrations, __ATOMIC_RELAXED);
    out->idle_ns = hal_tsc_to_ns(__atomic_load_n(&rq->idle_cycles, __ATOMIC_RELAXED));
    out->busy_ns = hal_tsc_to_ns(__atomic_load_n(&rq->busy_cycles, __ATOMIC_RELAXED));
    out->core = rq->core == HAL_CORE_P ? "P" : (rq->core == HAL_CORE_E ? "E" : "-");
    return 0;
}
