#include "proc/sched.h"
#include "proc/spinlock.h"
#include "lib/kprintf.h"

/* Run queue: singly-linked FIFO list */
static Thread *queue_head = NULL;
static Thread *queue_tail = NULL;
static Thread *idle_thread = NULL;
static Spinlock sched_lock = SPINLOCK_INIT;

static void queue_push(Thread *t) {
    t->next = NULL;
    if (queue_tail) {
        queue_tail->next = t;
    } else {
        queue_head = t;
    }
    queue_tail = t;
}

static Thread *queue_pop(void) {
    if (queue_head == NULL) return NULL;
    Thread *t = queue_head;
    queue_head = t->next;
    if (queue_head == NULL) {
        queue_tail = NULL;
    }
    t->next = NULL;
    return t;
}

void sched_init(void) {
    queue_head = NULL;
    queue_tail = NULL;
    idle_thread = NULL;
    kprintf("[SCHED] Scheduler initialized (round-robin)\n");
}

void sched_add_thread(Thread *t) {
    t->state = THREAD_READY;
    queue_push(t);
}

void sched_remove_thread(Thread *t) {
    /* Remove t from the run queue if present */
    Thread *prev = NULL;
    Thread *cur = queue_head;
    while (cur) {
        if (cur == t) {
            if (prev) {
                prev->next = cur->next;
            } else {
                queue_head = cur->next;
            }
            if (cur == queue_tail) {
                queue_tail = prev;
            }
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

void sched_schedule(void) {
    Thread *old = thread_current();
    Thread *next = queue_pop();

    if (next == NULL) {
        /* No threads in run queue */
        if (old->state == THREAD_RUNNING) {
            return; /* Current thread keeps running */
        }
        /* Current can't run — fall back to idle thread */
        next = idle_thread;
        if (next == NULL) return; /* Nothing to do */
    }

    /* Re-enqueue old thread if it's still runnable (not idle — idle only runs
     * when the queue is empty, so it should never occupy a queue slot). */
    if (old->state == THREAD_RUNNING && old != idle_thread) {
        old->state = THREAD_READY;
        queue_push(old);
    }

    next->state = THREAD_RUNNING;
    thread_set_current(next);

    if (next != old) {
        context_switch(&old->context, &next->context);
    }
}

void sched_yield(void) {
    spinlock_acquire(&sched_lock);
    sched_schedule();
    spinlock_release(&sched_lock);
}

void sched_set_idle_thread(Thread *t) {
    idle_thread = t;
    t->state = THREAD_RUNNING;
}
