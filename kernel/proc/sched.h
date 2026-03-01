#ifndef ARCHOS_PROC_SCHED_H
#define ARCHOS_PROC_SCHED_H

#include "proc/thread.h"

/* Initialize the scheduler. Must be called after thread_init(). */
void sched_init(void);

/* Add a thread to the run queue. */
void sched_add_thread(Thread *t);

/* Remove a thread from the run queue. */
void sched_remove_thread(Thread *t);

/* Cooperative yield: disable interrupts, schedule, re-enable. */
void sched_yield(void);

/* Core scheduling function: pick next thread, context switch.
 * Must be called with interrupts disabled. */
void sched_schedule(void);

/* Set the idle thread (runs when run queue is empty). */
void sched_set_idle_thread(Thread *t);

#endif /* ARCHOS_PROC_SCHED_H */
