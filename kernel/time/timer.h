#ifndef ARCHOS_TIME_TIMER_H
#define ARCHOS_TIME_TIMER_H

#include <stdint.h>
#include "lib/rbtree.h"

/* One-shot kernel timers on per-CPU queues (M1-06, ADR-0017).
 *
 * A timer runs fn(arg) once the clock (time/clock.h, ns) reaches its expiry,
 * in the timer interrupt of the CPU whose queue it is on, with interrupts off
 * and no lock held. Each CPU programs only its earliest expiry into its local
 * APIC timer; with nothing queued it has no timer interrupt at all.
 *
 * Rules:
 *  - A callback must not sleep, must not cancel_sync its own timer, and if it
 *    re-arms a timer it picks a time in the future.
 *  - One owner arms and cancels a given timer at a time (its own lock, or the
 *    thread the timer belongs to); the timer code orders that owner against
 *    the timer interrupt, not two owners against each other.
 *  - The memory of a timer may be reused only after timer_cancel_sync. */

typedef void (*TimerFn)(void *arg);

struct TimerBase;

typedef struct Timer {
    RbNode node;
    uint64_t expires;          /* clock ns */
    TimerFn fn;
    void *arg;
    struct TimerBase *base;    /* queue it was last armed on; NULL: never armed */
    uint8_t pending;           /* on base's queue (under its lock) */
} Timer;

/* Expiry that never comes: arming with it only removes the timer. */
#define TIMER_NEVER UINT64_MAX

/* Set up a timer (not armed). */
void timer_init(Timer *t, TimerFn fn, void *arg);

/* Arm t to run at clock time expires on this CPU's queue, first removing it
 * from wherever it is pending. Any context. O(log n). */
void timer_arm(Timer *t, uint64_t expires);

/* Remove t if pending. Returns 1 if it was, 0 if not (already ran, running,
 * or never armed). Does not wait for a running callback. Any context. */
int timer_cancel(Timer *t);

/* timer_cancel, then wait until a callback of t that is running on another
 * CPU has returned. Never from t's own callback, never holding a lock that
 * callback takes. */
void timer_cancel_sync(Timer *t);

/* 1 once this CPU has a timer queue (timer_init_boot / timer_init_cpus). */
int timer_cpu_ready(void);

/* 1 while t is queued. A hint without the owner's serialisation. */
int timer_pending(const Timer *t);

/* Sleep the calling thread until clock time deadline. Returns 0 once it is
 * reached, 1 if something else woke the thread first (sched_wake; signals,
 * M1-17). No spinlock may be held. */
int timer_sleep_until(uint64_t deadline);

/* The clock-event interrupt: run every expired timer of this CPU, program
 * the next expiry, then let the scheduler act on a slice that ended. */
void timer_interrupt(void);

/* Bring-up. timer_init_boot: the BSP's queue and the clock-event hardware
 * (after the TSC is calibrated and the local APIC is on; one_shot: LAPIC
 * one-shot mode even where TSC-deadline exists). timer_init_cpus:
 * a queue for every other online CPU, each CPU's timer programmed in place
 * (IPI), before any device interrupt can arm a timer there. */
void timer_init_boot(int one_shot);
void timer_init_cpus(void);

/* Statistics for /proc and the boot report: timer interrupts this CPU
 * handled, and timers that ran. */
typedef struct {
    uint64_t interrupts;
    uint64_t expired;
    uint32_t queued;
} TimerStats;
int timer_cpu_stats(uint32_t cpu, TimerStats *out);

#endif /* ARCHOS_TIME_TIMER_H */
