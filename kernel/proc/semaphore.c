#include "proc/semaphore.h"
#include "proc/sched.h"

void sem_init(Semaphore *s, int32_t initial_count) {
    s->guard = (Spinlock)SPINLOCK_INIT;
    s->count = initial_count;
    wq_init(&s->waiters);
}

void sem_wait(Semaphore *s) {
    spinlock_acquire(&s->guard);

    while (s->count <= 0) {
        /* Sleep releases guard atomically after enqueuing */
        wq_sleep(&s->waiters, &s->guard);
        spinlock_acquire(&s->guard);
    }

    s->count--;
    spinlock_release(&s->guard);
}

void sem_post(Semaphore *s) {
    spinlock_acquire(&s->guard);
    s->count++;
    wq_wake(&s->waiters);
    spinlock_release(&s->guard);
}

int sem_trywait(Semaphore *s) {
    spinlock_acquire(&s->guard);
    if (s->count <= 0) {
        spinlock_release(&s->guard);
        return -1;
    }
    s->count--;
    spinlock_release(&s->guard);
    return 0;
}

int32_t sem_getvalue(Semaphore *s) {
    spinlock_acquire(&s->guard);
    int32_t val = s->count;
    spinlock_release(&s->guard);
    return val;
}
