/* arc_os — Host-side tests for kernel/time/timer.c (M1-06)
 *
 * The real timer queues (and the real rbtree.c, built by test_rbtree.c) over
 * a fake clock and fake per-CPU clock-event hardware: test_cpu says which
 * simulated CPU the code runs on, and each CPU records what its timer was
 * last programmed to. The scheduler is a recorder; sched_schedule can "run"
 * the timer interrupt to play out a sleep. */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>

/* Guard kernel headers that need stubbing */
#define ARCHOS_ARCH_HAL_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_PANIC_H
#define ARCHOS_TIME_CLOCK_H

/* Unique name: test_thread.c links the real thread.c */
#define thread_current timer_test_thread_current

#include "proc/thread.h"
#include "proc/spinlock.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}
static int panics;
#define KERNEL_PANIC() (panics++)

/* time/clock.h */
#define NSEC_PER_SEC ((uint64_t)1000000000)
static uint64_t fake_now;
static uint64_t clock_mono_ns(void) {
    return fake_now;
}

/* mm/kmalloc.h */
#define GFP_ZERO 0x01
static void *kmalloc(size_t size, uint32_t flags) {
    (void)flags;
    return calloc(1, size);
}

/* ---- Simulated CPUs and their clock-event hardware (hal.h) ---- */

#define HAL_MAX_CPUS 4
static uint32_t test_cpu;
static int cpus_online = 1;
static void *cpu_timers[HAL_MAX_CPUS];
static uint64_t hw_expires[HAL_MAX_CPUS];   /* 0: stopped */
static int hw_writes[HAL_MAX_CPUS];
static int hw_inits[HAL_MAX_CPUS];
static void (*event_fn)(void);
static int event_one_shot;

static inline uint32_t hal_cpu_index(void) {
    return test_cpu;
}
static inline int hal_cpu_online(uint32_t cpu) {
    return cpu < (uint32_t)cpus_online;
}
static inline void *hal_this_cpu_timers(void) {
    return cpu_timers[test_cpu];
}
static inline void *hal_cpu_timers(uint32_t cpu) {
    return cpu < HAL_MAX_CPUS ? cpu_timers[cpu] : NULL;
}
static inline void hal_cpu_set_timers(uint32_t cpu, void *b) {
    cpu_timers[cpu] = b;
}
static inline int hal_clockevent_available(void) {
    return 1;
}
static inline void hal_clockevent_init(void (*fn)(void), int one_shot) {
    event_fn = fn;
    event_one_shot = one_shot;
}
static inline void hal_clockevent_cpu_init(void) {
    hw_inits[test_cpu]++;
}
static inline void hal_clockevent_program(uint64_t ns) {
    hw_expires[test_cpu] = ns;
    hw_writes[test_cpu]++;
}
static inline void hal_clockevent_stop(void) {
    hw_expires[test_cpu] = 0;
    hw_writes[test_cpu]++;
}
static inline uint64_t hal_clock_ns_to_cycles(uint64_t ns) {
    return ns;
}
/* ipi_call_all: every online CPU runs fn; returns how many others did */
static inline uint32_t hal_run_on_all_cpus(void (*fn)(void *), void *arg, uint64_t max_cycles) {
    (void)max_cycles;
    uint32_t self = test_cpu, others = 0;
    for (uint32_t c = 0; c < (uint32_t)cpus_online; c++) {
        test_cpu = c;
        fn(arg);
        if (c != self) others++;
    }
    test_cpu = self;
    return others;
}

/* ---- Scheduler and thread recorders (sched.h, thread.h) ---- */

static Thread self_thread;
Thread *timer_test_thread_current(void) {
    return &self_thread;
}
static int wakes, schedules, timer_exits;
static Thread *woken;
static uint64_t schedule_runs_until;   /* sched_schedule: advance the clock here and take the interrupt */
static void sched_wake(Thread *t) {
    wakes++;
    woken = t;
    t->state = THREAD_READY;
}
static void timer_interrupt(void);
static void sched_schedule(void) {
    schedules++;
    if (schedule_runs_until != 0) {
        fake_now = schedule_runs_until;
        timer_interrupt();
    }
}
static void sched_timer_exit(void) {
    timer_exits++;
}

#include "../kernel/time/timer.c"

/* ---- Fixtures ---- */

static TimerBase test_bases[HAL_MAX_CPUS];

static void reset(int cpus) {
    cpus_online = cpus;
    test_cpu = 0;
    fake_now = 1000;
    for (int c = 0; c < HAL_MAX_CPUS; c++) {
        base_init(&test_bases[c], (uint32_t)c);
        cpu_timers[c] = c < cpus ? &test_bases[c] : NULL;
        hw_expires[c] = 0;
        hw_writes[c] = 0;
        hw_inits[c] = 0;
    }
    wakes = schedules = timer_exits = panics = 0;
    woken = NULL;
    schedule_runs_until = 0;
    self_thread.state = THREAD_RUNNING;
}

#define N_LOG 16
static int fired_log[N_LOG], fired_n;
static void log_fn(void *arg) {
    if (fired_n < N_LOG) fired_log[fired_n++] = (int)(intptr_t)arg;
}

/* Take CPU cpu's timer interrupt at time now */
static void interrupt_at(uint32_t cpu, uint64_t now) {
    uint32_t saved = test_cpu;
    test_cpu = cpu;
    fake_now = now;
    timer_interrupt();
    test_cpu = saved;
}

/* ---- Tests ---- */

TEST(timer_fires_in_expiry_order) {
    reset(1);
    fired_n = 0;
    Timer a, b, c;
    timer_init(&a, log_fn, (void *)1);
    timer_init(&b, log_fn, (void *)2);
    timer_init(&c, log_fn, (void *)3);
    timer_arm(&a, 3000);
    timer_arm(&b, 1500);
    timer_arm(&c, 2000);
    ASSERT_EQ(hw_expires[0], 1500);   /* the earliest is in the hardware */
    interrupt_at(0, 1700);
    ASSERT_EQ(fired_n, 1);
    ASSERT_EQ(fired_log[0], 2);
    ASSERT_EQ(hw_expires[0], 2000);
    ASSERT_FALSE(timer_pending(&b));
    interrupt_at(0, 5000);   /* late: both due run in one interrupt, in order */
    ASSERT_EQ(fired_n, 3);
    ASSERT_EQ(fired_log[1], 3);
    ASSERT_EQ(fired_log[2], 1);
    ASSERT_EQ(hw_expires[0], 0);   /* empty queue: the timer is stopped (ADR-0017) */
    ASSERT_EQ(timer_exits, 2);     /* the scheduler looks after each interrupt */
    TimerStats st;
    ASSERT_EQ(timer_cpu_stats(0, &st), 0);
    ASSERT_EQ(st.interrupts, 2);
    ASSERT_EQ(st.expired, 3);
    ASSERT_EQ(st.queued, 0);
    return 0;
}

TEST(timer_equal_expiries_fifo) {
    reset(1);
    fired_n = 0;
    Timer t[4];
    for (int i = 0; i < 4; i++) {
        timer_init(&t[i], log_fn, (void *)(intptr_t)i);
        timer_arm(&t[i], 2000);
    }
    interrupt_at(0, 2000);
    ASSERT_EQ(fired_n, 4);
    for (int i = 0; i < 4; i++) ASSERT_EQ(fired_log[i], i);
    return 0;
}

TEST(timer_early_interrupt_reprograms) {
    reset(1);
    fired_n = 0;
    Timer a;
    timer_init(&a, log_fn, (void *)1);
    timer_arm(&a, 2000);
    interrupt_at(0, 1999);   /* the APIC one-shot fallback can fire a little early */
    ASSERT_EQ(fired_n, 0);
    ASSERT_TRUE(timer_pending(&a));
    ASSERT_EQ(hw_expires[0], 2000);
    interrupt_at(0, 2000);
    ASSERT_EQ(fired_n, 1);
    return 0;
}

TEST(timer_cancel_and_rearm) {
    reset(1);
    fired_n = 0;
    Timer a, b;
    timer_init(&a, log_fn, (void *)1);
    timer_init(&b, log_fn, (void *)2);
    ASSERT_EQ(timer_cancel(&a), 0);   /* never armed */
    timer_arm(&a, 2000);
    int writes = hw_writes[0];
    timer_arm(&b, 4000);
    ASSERT_EQ(hw_writes[0], writes);   /* a later timer leaves the hardware alone */
    ASSERT_EQ(timer_cancel(&a), 1);
    ASSERT_EQ(hw_expires[0], 4000);    /* the earliest went: the next is programmed */
    ASSERT_EQ(timer_cancel(&a), 0);
    timer_arm(&b, 3000);               /* re-arm moves it */
    ASSERT_EQ(hw_expires[0], 3000);
    timer_arm(&b, TIMER_NEVER);        /* only removes */
    ASSERT_FALSE(timer_pending(&b));
    ASSERT_EQ(hw_expires[0], 0);
    interrupt_at(0, 10000);
    ASSERT_EQ(fired_n, 0);
    return 0;
}

/* A timer is on the queue of the CPU that armed it last */
TEST(timer_per_cpu_queues) {
    reset(2);
    fired_n = 0;
    Timer a;
    timer_init(&a, log_fn, (void *)1);
    test_cpu = 1;
    timer_arm(&a, 2000);
    ASSERT_EQ(hw_expires[1], 2000);
    ASSERT_EQ(hw_expires[0], 0);
    interrupt_at(0, 3000);   /* CPU 0 has nothing due */
    ASSERT_EQ(fired_n, 0);
    test_cpu = 0;
    timer_arm(&a, 2500);     /* moves to CPU 0 */
    ASSERT_EQ(hw_expires[0], 2500);
    ASSERT_EQ(test_bases[1].queued, 0);
    ASSERT_EQ(test_bases[0].queued, 1);
    interrupt_at(1, 3000);   /* CPU 1's stale interrupt: nothing to run, its timer stops */
    ASSERT_EQ(fired_n, 0);
    ASSERT_EQ(hw_expires[1], 0);
    /* Cancel from another CPU */
    test_cpu = 1;
    ASSERT_EQ(timer_cancel(&a), 1);
    test_cpu = 0;
    ASSERT_EQ(test_bases[0].queued, 0);
    return 0;
}

static Timer rearm_timer;
static int rearm_runs;
static void rearm_fn(void *arg) {
    (void)arg;
    if (++rearm_runs < 3) timer_arm(&rearm_timer, fake_now + 100);
}

TEST(timer_callback_rearms_itself) {
    reset(1);
    rearm_runs = 0;
    timer_init(&rearm_timer, rearm_fn, NULL);
    timer_arm(&rearm_timer, 2000);
    interrupt_at(0, 2000);
    ASSERT_EQ(rearm_runs, 1);   /* not again in the same interrupt */
    ASSERT_EQ(hw_expires[0], 2100);
    interrupt_at(0, 2100);
    interrupt_at(0, 2200);
    ASSERT_EQ(rearm_runs, 3);
    ASSERT_EQ(hw_expires[0], 0);
    ASSERT_TRUE(test_bases[0].running == NULL);
    return 0;
}

TEST(timer_sleep_runs_to_deadline) {
    reset(1);
    int irq_was = hal_test_irq_enabled;
    ASSERT_EQ(timer_sleep_until(500), 0);   /* already past: no switch */
    ASSERT_EQ(schedules, 0);
    schedule_runs_until = 3000;             /* while away, the clock reaches the deadline */
    ASSERT_EQ(timer_sleep_until(3000), 0);
    ASSERT_EQ(schedules, 1);
    ASSERT_EQ(wakes, 1);
    ASSERT_TRUE(woken == &self_thread);
    ASSERT_EQ(test_bases[0].queued, 0);
    ASSERT_EQ(hw_expires[0], 0);
    ASSERT_EQ(hal_test_irq_enabled, irq_was);   /* interrupts as they were */
    return 0;
}

TEST(timer_sleep_woken_early) {
    reset(1);
    schedule_runs_until = 0;   /* something else wakes it before the deadline */
    ASSERT_EQ(timer_sleep_until(5000), 1);
    ASSERT_EQ(self_thread.state, THREAD_BLOCKED);   /* what the scheduler saw */
    ASSERT_EQ(wakes, 0);
    ASSERT_EQ(test_bases[0].queued, 0);   /* its stack timer is gone */
    ASSERT_EQ(hw_expires[0], 0);
    return 0;
}

TEST(timer_bring_up_programs_every_cpu) {
    cpus_online = 3;
    test_cpu = 0;
    for (int c = 0; c < HAL_MAX_CPUS; c++) {
        cpu_timers[c] = NULL;
        hw_inits[c] = 0;
        hw_expires[c] = 0;
    }
    panics = 0;
    ASSERT_EQ(timer_cpu_ready(), 0);
    timer_init_boot(1);
    ASSERT_EQ(timer_cpu_ready(), 1);
    ASSERT_EQ(event_one_shot, 1);
    ASSERT_TRUE(event_fn == clockevent_handler);
    ASSERT_EQ(hw_inits[0], 1);
    timer_init_cpus();
    ASSERT_EQ(panics, 0);
    ASSERT_TRUE(cpu_timers[1] != NULL && cpu_timers[2] != NULL && cpu_timers[3] == NULL);
    ASSERT_EQ(hw_inits[1], 1);
    ASSERT_EQ(hw_inits[2], 1);
    /* Each CPU's queue works on its own */
    fired_n = 0;
    Timer a;
    timer_init(&a, log_fn, (void *)7);
    test_cpu = 2;
    ASSERT_EQ(timer_cpu_ready(), 1);
    timer_arm(&a, fake_now + 10);
    test_cpu = 0;
    interrupt_at(2, fake_now + 10);
    ASSERT_EQ(fired_n, 1);
    ASSERT_EQ(fired_log[0], 7);
    TimerStats st;
    ASSERT_EQ(timer_cpu_stats(3, &st), -1);
    return 0;
}

TestCase timer_tests[] = {
    TEST_ENTRY(timer_fires_in_expiry_order),
    TEST_ENTRY(timer_equal_expiries_fifo),
    TEST_ENTRY(timer_early_interrupt_reprograms),
    TEST_ENTRY(timer_cancel_and_rearm),
    TEST_ENTRY(timer_per_cpu_queues),
    TEST_ENTRY(timer_callback_rearms_itself),
    TEST_ENTRY(timer_sleep_runs_to_deadline),
    TEST_ENTRY(timer_sleep_woken_early),
    TEST_ENTRY(timer_bring_up_programs_every_cpu),
};

int timer_test_count = sizeof(timer_tests) / sizeof(timer_tests[0]);
