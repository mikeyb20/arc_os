#include "proc/semaphore.h"
#include "proc/sched.h"

void sem_init(Semaphore *s, int32_t initial_count) {
    spin_lock_init(&s->guard, LOCK_LEVEL_OBJECT, "semaphore");
    s->count = initial_count;
    wq_init(&s->waiters);
}

void sem_wait(Semaphore *s) {
    lockdep_assert_can_sleep("sem_wait");
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    while (s->count <= 0) {
        /* Sleep releases guard atomically after enqueuing */
        wq_sleep(&s->waiters, &s->guard, flags);
        spin_lock_irqsave(&s->guard, &flags);
    }
    s->count--;
    spin_unlock_irqrestore(&s->guard, flags);
}

void sem_post(Semaphore *s) {
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    s->count++;
    wq_wake(&s->waiters);
    spin_unlock_irqrestore(&s->guard, flags);
}

int sem_trywait(Semaphore *s) {
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    if (s->count <= 0) {
        spin_unlock_irqrestore(&s->guard, flags);
        return -1;
    }
    s->count--;
    spin_unlock_irqrestore(&s->guard, flags);
    return 0;
}

int32_t sem_getvalue(Semaphore *s) {
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    int32_t val = s->count;
    spin_unlock_irqrestore(&s->guard, flags);
    return val;
}
