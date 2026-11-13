/* arc_os — Host-side tests for kernel/proc/sched.c (M1-05 SMP scheduler)
 *
 * The real sched.c runs against the real thread.h, sched.h and spinlock.h;
 * the HAL is stubbed with up to TEST_CPUS simulated CPUs. test_cpu says which
 * CPU the code under test runs on. context_switch is a recorder: when it
 * returns, sched_schedule finishes the switch the way the thread switched to
 * would (sched_finish_switch), so the state afterwards is the real one. */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_PROC_PROCESS_H
#define ARCHOS_PROC_KERNEL_LOCK_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_MM_VMM_H
#define ARCHOS_MM_MM_H
#define ARCHOS_ARCH_HAL_H

/* Unique names: test_thread.c links the real thread.c and its own switch stub */
#define thread_current     sched_test_thread_current
#define thread_set_current sched_test_thread_set_current
#define context_switch     sched_test_context_switch

#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/spinlock.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}
#define KERNEL_PANIC()                  \
    do {                                \
        printf("  sched.c panicked\n"); \
        abort();                        \
    } while (0)

/* process.h / mm.h: the scheduler only hands the next thread's mm to
 * mm_switch, which is stubbed below (its own tests are in test_mm.c) */
typedef struct Mm {
    uint64_t pgd;
} Mm;
typedef struct Process {
    Mm *mm;
} Process;

/* kmalloc.h / vmm.h (sched_start only) */
#define GFP_ZERO 0x01
static void *kmalloc(size_t size, uint32_t flags) {
    (void)flags;
    return calloc(1, size);
}
static void kfree(void *p) {
    free(p);
}
static uint64_t vmm_get_kernel_pml4(void) {
    return 0x1000;
}

/* kernel_lock.h: the switch gives the lock up and takes it back */
static int klock_held;
static int klock_suspends;
static int klock_resumed_with = -1;
static int kernel_lock_suspend(void) {
    klock_suspends++;
    int was = klock_held;
    klock_held = 0;
    return was;
}
static void kernel_lock_resume(int was) {
    klock_resumed_with = was;
    if (was) klock_held = 1;
}

/* ---- Simulated CPUs (hal.h) ---- */

#define HAL_MAX_CPUS   256
#define HAL_CORE_OTHER 0
#define HAL_CORE_P     1
#define HAL_CORE_E     2
#define TEST_CPUS      4

static uint32_t test_cpu;
static void *cpu_sched_ptr[TEST_CPUS];
static Thread *cpu_current[TEST_CPUS];
static int cpu_core[TEST_CPUS];
static int resched_ipis[TEST_CPUS];
static uint64_t fake_tsc;

static inline uint32_t hal_cpu_index(void) {
    return test_cpu;
}
static inline void *hal_this_cpu_sched(void) {
    return cpu_sched_ptr[test_cpu];
}
static inline void *hal_cpu_sched(uint32_t cpu) {
    return cpu < TEST_CPUS ? cpu_sched_ptr[cpu] : NULL;
}
static inline void hal_cpu_set_sched(uint32_t cpu, void *rq) {
    cpu_sched_ptr[cpu] = rq;
}
static inline int hal_cpu_online(uint32_t cpu) {
    return cpu < TEST_CPUS;
}
static inline int hal_cpu_core_kind(uint32_t cpu) {
    return cpu < TEST_CPUS ? cpu_core[cpu] : -1;
}
static inline void hal_send_resched(uint32_t cpu) {
    resched_ipis[cpu]++;
}
static inline uint64_t hal_tsc_read(void) {
    return fake_tsc;
}
static inline uint64_t hal_tsc_to_ns(uint64_t cycles) {
    return cycles;   /* 1 GHz */
}
static uint64_t cpu_cr3[TEST_CPUS];   /* what each simulated CPU has loaded */
static inline uint64_t hal_get_address_space(void) {
    return cpu_cr3[test_cpu];
}
static int kstack_sets;
static uint64_t kstack_last;
static inline void hal_set_kernel_stack(uint64_t rsp0) {
    kstack_sets++;
    kstack_last = rsp0;
}
static int cr3_writes;
static uint64_t cr3_last;
static inline void hal_switch_address_space(uint64_t cr3) {
    cr3_writes++;
    cr3_last = cr3;
    cpu_cr3[test_cpu] = cr3;
}
/* mm.h: mm_switch compares with this CPU's loaded mm (the real one is tested
 * in test_mm.c); the recorder checks what the scheduler hands it */
static Mm *cpu_mm[TEST_CPUS];
static int mm_switch_calls;
static Mm *mm_switch_last;
static void mm_switch(Mm *next) {
    mm_switch_calls++;
    mm_switch_last = next;
    if (cpu_mm[test_cpu] == next) return;
    cpu_mm[test_cpu] = next;
    hal_switch_address_space(next != NULL ? next->pgd : 0x1000);
}
static int timer_starts, timer_stops;
static inline void hal_slice_timer_start(uint64_t ns) {
    (void)ns;
    timer_starts++;
}
static inline void hal_slice_timer_stop(void) {
    timer_stops++;
}
static inline void hal_slice_timer_init(void (*fn)(void)) {
    (void)fn;
}
static inline void hal_release_aps(void) {}
static inline void hal_idle_wait(void) {}
static inline void hal_halt_forever(void) {}

/* thread.h: the running thread of the simulated CPU */
Thread *sched_test_thread_current(void) {
    return cpu_current[test_cpu];
}
void sched_test_thread_set_current(Thread *t) {
    cpu_current[test_cpu] = t;
}

/* context_switch recorder; the FPU/FS hook must run first, for the same pair */
static int ctx_switch_count;
static ThreadContext *ctx_switch_old, *ctx_switch_new;
static uint32_t ctx_switch_lock_depth;
static int ctx_switch_irq;
void sched_test_context_switch(ThreadContext *old, ThreadContext *new_ctx) {
    ctx_switch_count++;
    ctx_switch_old = old;
    ctx_switch_new = new_ctx;
    ctx_switch_lock_depth = hal_test_lock_depth;
    ctx_switch_irq = hal_test_irq_enabled;
}
static int arch_switch_count;
static Thread *arch_switch_old, *arch_switch_next;
static int arch_switch_before_ctx;
static void arch_thread_switch(Thread *old, Thread *next) {
    arch_switch_count++;
    arch_switch_old = old;
    arch_switch_next = next;
    arch_switch_before_ctx = (ctx_switch_count == 0);
}

#include "../kernel/proc/sched.c"

/* ---- Fixtures ---- */

#define POOL_SIZE 12
static Thread pool[POOL_SIZE];
static RunQueue ap_rq[TEST_CPUS];

static Thread *make_thread(int i, tid_t tid, uint8_t state) {
    memset(&pool[i], 0, sizeof(Thread));
    pool[i].tid = tid;
    pool[i].state = state;
    cpumask_fill(&pool[i].affinity);
    return &pool[i];
}

static void reset_sched_state(void) {
    memset(&bsp_rq, 0, sizeof(bsp_rq));
    memset(ap_rq, 0, sizeof(ap_rq));
    cpumask_clear(&online_mask);
    memset(core_masks, 0, sizeof(core_masks));
    sched_cpu_limit = 0;
    slice_timer_ready = 0;
    test_cpu = 0;
    memset(cpu_sched_ptr, 0, sizeof(cpu_sched_ptr));
    memset(cpu_current, 0, sizeof(cpu_current));
    memset(cpu_core, 0, sizeof(cpu_core));
    memset(resched_ipis, 0, sizeof(resched_ipis));
    memset(pool, 0, sizeof(pool));
    fake_tsc = 0;
    kstack_sets = 0;
    kstack_last = 0;
    cr3_writes = 0;
    cr3_last = 0;
    for (int i = 0; i < TEST_CPUS; i++) cpu_cr3[i] = 0x1000;   /* kernel PML4 */
    for (int i = 0; i < TEST_CPUS; i++) cpu_mm[i] = NULL;
    mm_switch_calls = 0;
    mm_switch_last = NULL;
    timer_starts = timer_stops = 0;
    ctx_switch_count = 0;
    ctx_switch_old = ctx_switch_new = NULL;
    ctx_switch_lock_depth = 99;
    ctx_switch_irq = -1;
    arch_switch_count = 0;
    arch_switch_old = arch_switch_next = NULL;
    arch_switch_before_ctx = 0;
    klock_held = 0;
    klock_suspends = 0;
    klock_resumed_with = -1;
    hal_test_irq_enabled = 0;   /* sched_schedule's callers turn interrupts off */
    hal_test_lock_depth = 0;
}

/* CPU 0 with `running` as its current thread (sched_init) */
static void boot_cpu(Thread *running) {
    running->state = THREAD_RUNNING;
    cpu_current[0] = running;
    sched_init();
}

/* Another CPU, running its idle thread (what sched_start + sched_ap_start set up) */
static RunQueue *start_cpu(uint32_t cpu, Thread *idle) {
    RunQueue *rq = &ap_rq[cpu];
    rq_init(rq, cpu);
    rq->idle = idle;
    rq->curr = idle;
    idle->state = THREAD_RUNNING;
    idle->on_cpu = 1;
    idle->cpu = cpu;
    cpumask_clear(&idle->affinity);
    cpumask_set(&idle->affinity, cpu);
    rq_online(rq);
    cpu_current[cpu] = idle;
    return rq;
}

/* All switches leave no spinlock held */
#define ASSERT_UNLOCKED(rq)                \
    do {                                   \
        ASSERT_EQ((rq)->lock.locked, 0);   \
        ASSERT_EQ(hal_test_lock_depth, 0); \
    } while (0)

/* ---- Queue and switch basics (one CPU) ---- */

static int test_sched_init(void) {
    reset_sched_state();
    Thread *boot = make_thread(0, 0, THREAD_RUNNING);
    boot_cpu(boot);
    ASSERT_TRUE(bsp_rq.curr == boot);
    ASSERT_EQ(boot->on_cpu, 1);
    ASSERT_EQ(boot->cpu, 0);
    ASSERT_TRUE(bsp_rq.head == NULL);
    ASSERT_TRUE(cpumask_test(&online_mask, 0));
    ASSERT_TRUE(sched_this_lockdep() == &bsp_rq.lockdep);
    ASSERT_EQ(bsp_rq.lock.level, LOCK_LEVEL_RUNQUEUE);
    ASSERT_EQ(bsp_rq.lock.subkey, 1);
    return 0;
}

static int test_sched_add_fifo(void) {
    reset_sched_state();
    boot_cpu(make_thread(0, 0, THREAD_RUNNING));
    Thread *a = make_thread(1, 1, THREAD_CREATED);
    Thread *b = make_thread(2, 2, THREAD_CREATED);
    Thread *c = make_thread(3, 3, THREAD_CREATED);
    sched_add_thread(a);
    sched_add_thread(b);
    sched_add_thread(c);
    ASSERT_TRUE(bsp_rq.head == a);
    ASSERT_TRUE(a->rq_next == b && b->rq_next == c && c->rq_next == NULL);
    ASSERT_TRUE(c->rq_prev == b && b->rq_prev == a && a->rq_prev == NULL);
    ASSERT_TRUE(bsp_rq.tail == c);
    ASSERT_EQ(bsp_rq.nr_queued, 3);
    ASSERT_EQ(a->state, THREAD_READY);
    ASSERT_EQ(a->on_rq, 1);
    ASSERT_UNLOCKED(&bsp_rq);
    return 0;
}

static int test_sched_schedule_basic(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_CREATED);
    boot_cpu(a);
    sched_add_thread(b);
    sched_schedule();

    ASSERT_TRUE(sched_test_thread_current() == b);
    ASSERT_TRUE(bsp_rq.curr == b);
    ASSERT_EQ(b->state, THREAD_RUNNING);
    ASSERT_EQ(b->on_rq, 0);
    ASSERT_EQ(b->on_cpu, 1);
    /* a is queued again and fully switched out */
    ASSERT_EQ(a->state, THREAD_READY);
    ASSERT_TRUE(bsp_rq.head == a);
    ASSERT_EQ(a->on_cpu, 0);
    /* The switch ran with the run-queue lock held and interrupts off */
    ASSERT_EQ(ctx_switch_count, 1);
    ASSERT_TRUE(ctx_switch_old == &a->context);
    ASSERT_TRUE(ctx_switch_new == &b->context);
    ASSERT_EQ(ctx_switch_lock_depth, 1);
    ASSERT_EQ(ctx_switch_irq, 0);
    ASSERT_EQ(arch_switch_count, 1);
    ASSERT_TRUE(arch_switch_old == a && arch_switch_next == b);
    ASSERT_TRUE(arch_switch_before_ctx);
    ASSERT_EQ(bsp_rq.nr_switches, 1);
    ASSERT_TRUE(bsp_rq.prev == NULL);
    ASSERT_UNLOCKED(&bsp_rq);
    return 0;
}

static int test_sched_schedule_empty_keeps_current(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    boot_cpu(a);
    sched_schedule();
    ASSERT_TRUE(sched_test_thread_current() == a);
    ASSERT_EQ(a->state, THREAD_RUNNING);
    ASSERT_EQ(a->on_rq, 0);
    ASSERT_EQ(ctx_switch_count, 0);
    ASSERT_UNLOCKED(&bsp_rq);
    return 0;
}

static int test_sched_blocked_runs_idle(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *idle = make_thread(1, 0, THREAD_READY);
    boot_cpu(a);
    bsp_rq.idle = idle;
    a->state = THREAD_BLOCKED;
    sched_schedule();
    ASSERT_TRUE(sched_test_thread_current() == idle);
    ASSERT_EQ(idle->state, THREAD_RUNNING);
    ASSERT_TRUE(bsp_rq.head == NULL);   /* a stays off every queue */
    ASSERT_EQ(a->on_rq, 0);
    ASSERT_EQ(a->on_cpu, 0);
    ASSERT_EQ(ctx_switch_count, 1);
    ASSERT_UNLOCKED(&bsp_rq);
    return 0;
}

static int test_sched_idle_not_requeued(void) {
    reset_sched_state();
    Thread *idle = make_thread(0, 0, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_CREATED);
    boot_cpu(idle);
    bsp_rq.idle = idle;
    sched_add_thread(b);
    sched_schedule();
    ASSERT_TRUE(sched_test_thread_current() == b);
    ASSERT_TRUE(bsp_rq.head == NULL && bsp_rq.tail == NULL);
    ASSERT_EQ(bsp_rq.nr_queued, 0);
    ASSERT_EQ(idle->on_rq, 0);
    return 0;
}

static int test_sched_yield(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_CREATED);
    boot_cpu(a);
    sched_add_thread(b);
    hal_test_irq_enabled = 1;
    sched_yield();
    ASSERT_TRUE(sched_test_thread_current() == b);
    ASSERT_EQ(ctx_switch_irq, 0);         /* switched with interrupts off */
    ASSERT_EQ(hal_test_irq_enabled, 1);   /* and they came back */
    return 0;
}

/* The address space follows the next thread's process; a thread with no
 * process runs on the kernel page tables (mm NULL) */
static int test_sched_cr3_from_thread_proc(void) {
    reset_sched_state();
    static Mm ma, mb;
    static Process pa, pb;
    ma.pgd = 0xA000;
    mb.pgd = 0xB000;
    pa.mm = &ma;
    pb.mm = &mb;
    Thread *a = make_thread(0, 100, THREAD_RUNNING);
    Thread *b = make_thread(1, 200, THREAD_CREATED);
    Thread *k = make_thread(2, 300, THREAD_CREATED);
    Thread *k2 = make_thread(3, 400, THREAD_CREATED);
    a->proc = &pa;
    b->proc = &pb;
    boot_cpu(a);
    cpu_mm[0] = &ma;
    a->state = THREAD_DEAD;   /* exiting: never queued again */
    sched_add_thread(b);
    sched_schedule();         /* a -> b */
    ASSERT_TRUE(mm_switch_last == &mb);
    ASSERT_EQ(cr3_last, 0xB000);
    ASSERT_TRUE(bsp_rq.head == NULL);
    b->state = THREAD_DEAD;
    sched_add_thread(k);
    sched_schedule();         /* b -> k (no process) */
    ASSERT_TRUE(mm_switch_last == NULL);
    ASSERT_EQ(cr3_last, 0x1000);
    k->state = THREAD_DEAD;
    int writes = cr3_writes;
    sched_add_thread(k2);
    sched_schedule();         /* k -> k2: same page tables, no write */
    ASSERT_EQ(cr3_writes, writes);
    return 0;
}

/* The scheduler keeps no CR3 cache of its own: every switch goes through
 * mm_switch, which compares mm pointers. An exec'd process has a new mm, so
 * a reused PML4 can't be skipped (M1-05 review finding 1) */
static int test_sched_cr3_after_exec(void) {
    reset_sched_state();
    static Mm ma, mc;
    static Process pa, pc;
    ma.pgd = 0xA000;
    pa.mm = &ma;
    Thread *boot = make_thread(0, 100, THREAD_RUNNING);
    Thread *a = make_thread(1, 200, THREAD_CREATED);
    Thread *c = make_thread(2, 300, THREAD_CREATED);
    a->proc = &pa;
    boot_cpu(boot);
    boot->state = THREAD_DEAD;
    sched_add_thread(a);
    int calls = mm_switch_calls;
    sched_schedule();         /* boot -> a */
    ASSERT_EQ(mm_switch_calls, calls + 1);
    ASSERT_EQ(cpu_cr3[0], 0xA000);
    static Mm mnew;
    mnew.pgd = 0xC000;        /* a execs: new mm, switched to by exec itself */
    pa.mm = &mnew;
    mm_switch(&mnew);
    mc.pgd = 0xA000;          /* a fork child gets the freed PML4 back */
    pc.mm = &mc;
    c->proc = &pc;
    a->state = THREAD_DEAD;
    sched_add_thread(c);
    int before = cr3_writes;
    sched_schedule();         /* a -> c must load 0xA000 again */
    ASSERT_TRUE(mm_switch_last == &mc);
    ASSERT_EQ(cr3_writes, before + 1);
    ASSERT_EQ(cpu_cr3[0], 0xA000);
    return 0;
}

/* Every switch points this CPU's entry stacks at the next thread's kernel
 * stack; a thread without one (the boot thread) leaves them as they are */
static int test_sched_kernel_stack_follows_thread(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 100, THREAD_RUNNING);
    Thread *b = make_thread(1, 200, THREAD_CREATED);
    Thread *c = make_thread(2, 300, THREAD_CREATED);
    b->kernel_stack_top = 0xB0000;
    boot_cpu(a);
    sched_add_thread(b);
    sched_schedule();         /* a -> b */
    ASSERT_EQ(kstack_sets, 1);
    ASSERT_EQ(kstack_last, 0xB0000);
    b->state = THREAD_DEAD;
    sched_add_thread(c);
    sched_schedule();         /* b -> c: no kernel stack */
    ASSERT_EQ(kstack_sets, 1);
    return 0;
}

/* A thread holding the kernel lock gives it up across the switch and gets it
 * back when it runs again */
static int test_sched_kernel_lock_across_switch(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_CREATED);
    boot_cpu(a);
    sched_add_thread(b);
    klock_held = 1;
    sched_schedule();
    ASSERT_EQ(klock_suspends, 1);
    ASSERT_EQ(klock_resumed_with, 1);
    ASSERT_EQ(klock_held, 1);
    return 0;
}

/* The slice timer runs only while a real thread runs */
static int test_sched_slice_timer_only_when_busy(void) {
    reset_sched_state();
    Thread *idle = make_thread(0, 0, THREAD_RUNNING);
    Thread *a = make_thread(1, 1, THREAD_CREATED);
    boot_cpu(idle);
    bsp_rq.idle = idle;
    slice_timer_ready = 1;
    sched_add_thread(a);
    sched_schedule();          /* idle -> a */
    ASSERT_EQ(timer_starts, 1);
    ASSERT_EQ(timer_stops, 0);
    a->state = THREAD_BLOCKED;
    sched_schedule();          /* a -> idle */
    ASSERT_EQ(timer_stops, 1);
    return 0;
}

/* Never preempt a CPU that holds a spinlock */
static int test_sched_preempt_skips_lock_holder(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_CREATED);
    boot_cpu(a);
    sched_add_thread(b);
    hal_test_lock_depth = 1;
    sched_preempt();
    ASSERT_EQ(ctx_switch_count, 0);
    hal_test_lock_depth = 0;
    sched_preempt();
    ASSERT_EQ(ctx_switch_count, 1);
    ASSERT_TRUE(sched_test_thread_current() == b);
    return 0;
}

/* Busy and idle time per CPU (/proc/sched_debug) */
static int test_sched_time_accounting(void) {
    reset_sched_state();
    Thread *idle = make_thread(0, 0, THREAD_RUNNING);
    Thread *a = make_thread(1, 1, THREAD_CREATED);
    boot_cpu(idle);
    bsp_rq.idle = idle;
    fake_tsc = 100;
    sched_add_thread(a);
    sched_schedule();          /* idle ran 100 */
    fake_tsc = 400;
    a->state = THREAD_BLOCKED;
    sched_schedule();          /* a ran 300 */
    SchedCpuStats st;
    ASSERT_EQ(sched_cpu_stats(0, &st), 0);
    ASSERT_EQ(st.idle_ns, 100);
    ASSERT_EQ(st.busy_ns, 300);
    ASSERT_EQ(st.nr_switches, 2);
    ASSERT_EQ(st.nr_running, 0);
    ASSERT_STR_EQ(st.core, "-");
    ASSERT_EQ(sched_cpu_stats(1, &st), -1);   /* no run queue there */
    return 0;
}

/* ---- Several CPUs ---- */

/* A wakeup goes to an idle CPU and kicks it; the busy CPU is left alone */
static int test_sched_wake_picks_idle_cpu(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *idle1 = make_thread(1, 0, THREAD_RUNNING);
    Thread *t = make_thread(2, 5, THREAD_BLOCKED);
    boot_cpu(a);
    start_cpu(1, idle1);
    t->cpu = 0;   /* last ran on the busy CPU */
    ASSERT_EQ(sched_wake(t), 1);
    ASSERT_EQ(t->cpu, 1);
    ASSERT_TRUE(ap_rq[1].head == t);
    ASSERT_EQ(t->state, THREAD_READY);
    ASSERT_EQ(resched_ipis[1], 1);
    ASSERT_EQ(resched_ipis[0], 0);
    ASSERT_UNLOCKED(&ap_rq[1]);
    return 0;
}

/* The CPU it last ran on wins when that one is idle (cache stays warm) */
static int test_sched_wake_prefers_last_idle_cpu(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *i1 = make_thread(1, 0, THREAD_RUNNING);
    Thread *i2 = make_thread(2, 0, THREAD_RUNNING);
    Thread *t = make_thread(3, 5, THREAD_BLOCKED);
    boot_cpu(a);
    start_cpu(1, i1);
    start_cpu(2, i2);
    t->cpu = 2;
    sched_wake(t);
    ASSERT_EQ(t->cpu, 2);
    return 0;
}

/* Else the least loaded allowed CPU */
static int test_sched_wake_least_loaded(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *i1 = make_thread(1, 0, THREAD_RUNNING);
    Thread *i2 = make_thread(2, 0, THREAD_RUNNING);
    Thread *q1 = make_thread(3, 6, THREAD_CREATED);
    Thread *q2 = make_thread(4, 7, THREAD_CREATED);
    Thread *q3 = make_thread(5, 8, THREAD_CREATED);
    Thread *t = make_thread(6, 9, THREAD_BLOCKED);
    boot_cpu(a);   /* load 1 */
    start_cpu(1, i1);
    start_cpu(2, i2);
    rq_enqueue(&ap_rq[1], q1);   /* CPU 1: load 2 */
    rq_enqueue(&ap_rq[1], q2);
    rq_enqueue(&ap_rq[2], q3);   /* CPU 2: load 1, ties with CPU 0 */
    t->cpu = 1;
    sched_wake(t);
    ASSERT_EQ(t->cpu, 0);        /* lowest of the least loaded */
    return 0;
}

static int test_sched_wake_respects_affinity(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *idle1 = make_thread(1, 0, THREAD_RUNNING);
    Thread *t = make_thread(2, 5, THREAD_BLOCKED);
    boot_cpu(a);
    start_cpu(1, idle1);
    cpumask_clear(&t->affinity);
    cpumask_set(&t->affinity, 0);
    sched_wake(t);
    ASSERT_EQ(t->cpu, 0);        /* busy, but the only CPU allowed */
    ASSERT_TRUE(bsp_rq.head == t);
    ASSERT_EQ(resched_ipis[1], 0);
    return 0;
}

/* A sleeper that a waker reaches before it has switched away keeps running */
static int test_sched_wake_before_switch_out(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *idle1 = make_thread(1, 0, THREAD_RUNNING);
    Thread *t = make_thread(2, 5, THREAD_RUNNING);
    boot_cpu(a);
    RunQueue *rq1 = start_cpu(1, idle1);
    rq1->curr = t;               /* t runs on CPU 1 ... */
    cpu_current[1] = t;
    t->cpu = 1;
    t->on_cpu = 1;
    t->state = THREAD_BLOCKED;   /* ... and has just marked itself asleep */
    ASSERT_EQ(sched_wake(t), 1);
    ASSERT_EQ(t->state, THREAD_RUNNING);
    ASSERT_EQ(t->on_rq, 0);
    ASSERT_EQ(rq1->nr_queued, 0);
    ASSERT_EQ(bsp_rq.nr_queued, 0);
    /* Its sched_schedule now sees RUNNING and carries on */
    test_cpu = 1;
    sched_schedule();
    ASSERT_TRUE(cpu_current[1] == t);
    ASSERT_EQ(t->state, THREAD_RUNNING);
    return 0;
}

/* A wakeup only wakes a blocked thread, and only once */
static int test_sched_wake_once(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *t = make_thread(1, 5, THREAD_BLOCKED);
    Thread *r = make_thread(2, 6, THREAD_RUNNING);
    boot_cpu(a);
    ASSERT_EQ(sched_wake(t), 1);
    ASSERT_EQ(sched_wake(t), 0);
    ASSERT_EQ(bsp_rq.nr_queued, 1);
    ASSERT_EQ(sched_wake(r), 0);
    ASSERT_EQ(sched_continue(t), 0);   /* READY, not STOPPED */
    Thread *s = make_thread(3, 7, THREAD_STOPPED);
    ASSERT_EQ(sched_wake(s), 0);       /* SIGCONT's job, not a wakeup's */
    ASSERT_EQ(sched_continue(s), 1);
    ASSERT_EQ(bsp_rq.nr_queued, 2);
    return 0;
}

/* An idle CPU pulls from the busiest queue, from the tail, skipping threads
 * whose affinity excludes it */
static int test_sched_idle_pull(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *idle1 = make_thread(1, 0, THREAD_RUNNING);
    Thread *q[3];
    boot_cpu(a);
    RunQueue *rq1 = start_cpu(1, idle1);
    for (int i = 0; i < 3; i++) {
        q[i] = make_thread(2 + i, 10 + i, THREAD_CREATED);
        rq_enqueue(&bsp_rq, q[i]);
    }
    cpumask_clear(&q[2]->affinity);   /* the tail must stay on CPU 0 */
    cpumask_set(&q[2]->affinity, 0);
    test_cpu = 1;
    sched_pull(rq1);
    ASSERT_EQ(rq1->nr_queued, 1);
    ASSERT_TRUE(rq1->head == q[1]);
    ASSERT_EQ(q[1]->cpu, 1);
    ASSERT_EQ(bsp_rq.nr_queued, 2);
    ASSERT_TRUE(bsp_rq.head == q[0] && q[0]->rq_next == q[2] && bsp_rq.tail == q[2]);
    ASSERT_EQ(rq1->nr_migrations, 1);
    ASSERT_UNLOCKED(rq1);
    ASSERT_UNLOCKED(&bsp_rq);
    /* The pulled thread runs there */
    sched_schedule();
    ASSERT_TRUE(cpu_current[1] == q[1]);
    return 0;
}

/* Affinity: a queued thread moves at once */
static int test_sched_affinity_moves_queued(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *idle1 = make_thread(1, 0, THREAD_RUNNING);
    Thread *t = make_thread(2, 5, THREAD_CREATED);
    boot_cpu(a);
    start_cpu(1, idle1);
    rq_enqueue(&bsp_rq, t);
    CpuMask m;
    cpumask_clear(&m);
    cpumask_set(&m, 1);
    ASSERT_EQ(sched_set_affinity(t, &m), 0);
    ASSERT_EQ(t->cpu, 1);
    ASSERT_EQ(bsp_rq.nr_queued, 0);
    ASSERT_TRUE(ap_rq[1].head == t);
    ASSERT_EQ(resched_ipis[1], 1);
    /* A mask with no online CPU is refused */
    cpumask_clear(&m);
    cpumask_set(&m, 3);
    ASSERT_EQ(sched_set_affinity(t, &m), -1);
    return 0;
}

/* Affinity: the running thread moves itself at once (it yields) and is
 * queued on an allowed CPU only after it has switched out */
static int test_sched_affinity_moves_running_self(void) {
    reset_sched_state();
    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *idle0 = make_thread(1, 0, THREAD_READY);
    Thread *idle1 = make_thread(2, 0, THREAD_RUNNING);
    boot_cpu(a);
    bsp_rq.idle = idle0;
    start_cpu(1, idle1);
    CpuMask m;
    cpumask_clear(&m);
    cpumask_set(&m, 1);
    ASSERT_EQ(sched_set_affinity(a, &m), 0);
    ASSERT_TRUE(cpu_current[0] == idle0);   /* CPU 0 runs its idle thread */
    ASSERT_EQ(a->on_cpu, 0);
    ASSERT_EQ(a->cpu, 1);
    ASSERT_TRUE(ap_rq[1].head == a);
    ASSERT_EQ(a->state, THREAD_READY);
    ASSERT_TRUE(bsp_rq.migrate == NULL);
    ASSERT_EQ(resched_ipis[1], 1);
    ASSERT_UNLOCKED(&bsp_rq);
    return 0;
}

/* P-core and E-core masks follow the HAL's core kinds */
static int test_sched_core_masks(void) {
    reset_sched_state();
    cpu_core[0] = HAL_CORE_P;
    cpu_core[1] = HAL_CORE_E;
    boot_cpu(make_thread(0, 0, THREAD_RUNNING));
    start_cpu(1, make_thread(1, 0, THREAD_RUNNING));
    CpuMask p, e, other;
    sched_core_mask(SCHED_CORE_P, &p);
    sched_core_mask(SCHED_CORE_E, &e);
    sched_core_mask(0, &other);
    ASSERT_TRUE(cpumask_test(&p, 0) && !cpumask_test(&p, 1));
    ASSERT_TRUE(cpumask_test(&e, 1) && !cpumask_test(&e, 0));
    ASSERT_FALSE(cpumask_test(&other, 0));
    SchedCpuStats st;
    sched_cpu_stats(1, &st);
    ASSERT_STR_EQ(st.core, "E");
    return 0;
}

TestCase sched_tests[] = {
    {"init", test_sched_init},
    {"add_fifo", test_sched_add_fifo},
    {"schedule_basic", test_sched_schedule_basic},
    {"schedule_empty_keeps_cur", test_sched_schedule_empty_keeps_current},
    {"blocked_runs_idle", test_sched_blocked_runs_idle},
    {"idle_not_requeued", test_sched_idle_not_requeued},
    {"yield", test_sched_yield},
    {"cr3_from_thread_proc", test_sched_cr3_from_thread_proc},
    {"cr3_after_exec", test_sched_cr3_after_exec},
    {"kernel_stack_follows_thread", test_sched_kernel_stack_follows_thread},
    {"kernel_lock_across_switch", test_sched_kernel_lock_across_switch},
    {"slice_timer_only_when_busy", test_sched_slice_timer_only_when_busy},
    {"preempt_skips_lock_holder", test_sched_preempt_skips_lock_holder},
    {"time_accounting", test_sched_time_accounting},
    {"wake_picks_idle_cpu", test_sched_wake_picks_idle_cpu},
    {"wake_prefers_last_idle_cpu", test_sched_wake_prefers_last_idle_cpu},
    {"wake_least_loaded", test_sched_wake_least_loaded},
    {"wake_respects_affinity", test_sched_wake_respects_affinity},
    {"wake_before_switch_out", test_sched_wake_before_switch_out},
    {"wake_once", test_sched_wake_once},
    {"idle_pull", test_sched_idle_pull},
    {"affinity_moves_queued", test_sched_affinity_moves_queued},
    {"affinity_moves_running_self", test_sched_affinity_moves_running_self},
    {"core_masks", test_sched_core_masks},
};

int sched_test_count = sizeof(sched_tests) / sizeof(sched_tests[0]);
