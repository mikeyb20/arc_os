#ifndef ARCHOS_PROC_SCHED_H
#define ARCHOS_PROC_SCHED_H

/* SMP scheduler (M1-05): one run queue per CPU, round robin with time slices.
 * See proc/sched.c for the invariants and docs/kernel/locking.md for the locks. */

#include <stdint.h>
#include "proc/thread.h"
#include "proc/cpumask.h"
#include "proc/lockdep.h"

/* Length of a time slice */
#define SCHED_SLICE_NS 100000000ULL   /* 100 ms, the old 10 PIT ticks */

/* The boot CPU's run queue, with the running boot thread as its current
 * thread. Call once after thread_init(). */
void sched_init(void);

/* Make a new thread runnable (never ran, on no queue) on a CPU chosen like a
 * wakeup. */
void sched_add_thread(Thread *t);

/* Wake a THREAD_BLOCKED thread: it runs again on a CPU chosen by
 * sched_select_cpu. Safe against the thread still being on its way to
 * sleep. Returns 1 if it was woken, 0 if it was not blocked. */
int sched_wake(Thread *t);

/* Resume a THREAD_STOPPED thread (SIGCONT). Returns 1 if it was stopped. */
int sched_continue(Thread *t);

/* Give up the CPU: interrupts off, schedule, interrupts back. */
void sched_yield(void);

/* Pick the next thread on this CPU and switch to it. Interrupts must be off.
 * The current thread is queued again if it is still runnable; otherwise it
 * stays off every queue until someone wakes it. Gives up the kernel lock
 * across the switch (proc/kernel_lock.h). */
void sched_schedule(void);

/* Slice timer and reschedule IPI: schedule now unless this CPU holds a
 * spinlock (then the next slice end tries again). Interrupt context. */
void sched_preempt(void);

/* First thing a thread runs after it is switched to for the first time
 * (thread_trampoline): finishes the switch that started it. */
void sched_finish_switch(void);

/* BSP: run queues and idle threads for the other online CPUs, the slice
 * timer, then the APs are let into their schedulers. The boot thread stays
 * pinned to CPU 0. Runs once (later calls return at once): sched_start calls
 * it, and the diagnostic self-test calls it earlier to put threads on every
 * CPU. */
void sched_start_aps(void);

/* BSP, end of boot: sched_start_aps, then the boot thread becomes CPU 0's
 * idle thread. Does not return. */
void sched_start(void) __attribute__((noreturn));

/* AP, once sched_start let it in: switch to this CPU's idle thread. Does not
 * return. */
void sched_ap_start(void) __attribute__((noreturn));

/* Limit t to the CPUs in mask (must include an online CPU, else -1). A queued
 * thread moves at once; a running one moves itself at its next schedule. */
int sched_set_affinity(Thread *t, const CpuMask *mask);

/* The CPUs of one core type: SCHED_CORE_P or SCHED_CORE_E (empty on CPUs
 * that are not hybrid). */
#define SCHED_CORE_P 1
#define SCHED_CORE_E 2
void sched_core_mask(int core, CpuMask *out);

/* Wait until t has switched away for the last time (t is DEAD): its stack and
 * TCB may then be freed. Short: t is at most finishing its last switch. */
void sched_wait_off_cpu(Thread *t);

/* This CPU's lock-order checker state; NULL before its run queue exists. */
LockdepCpu *sched_this_lockdep(void);

/* Per-CPU statistics (/proc/sched_debug). */
typedef struct {
    uint32_t nr_running;     /* queued + running (idle not counted) */
    uint64_t nr_switches;    /* context switches */
    uint64_t nr_migrations;  /* threads pulled here by an idle CPU's balancing */
    uint64_t idle_ns;        /* time in the idle thread, up to the last switch */
    uint64_t busy_ns;        /* time running other threads, up to the last switch */
    const char *core;        /* "P", "E" or "-" */
} SchedCpuStats;

/* Fill out for CPU cpu. Returns 0, or -1 if that CPU has no run queue. */
int sched_cpu_stats(uint32_t cpu, SchedCpuStats *out);

#endif /* ARCHOS_PROC_SCHED_H */
