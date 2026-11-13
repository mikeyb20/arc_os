/* arc_os — sleeping read-write lock (rwsem.h, M1-07)
 *
 * Owns: the reader count, the writer and the waiter queue of one RwSem.
 * Invariants: readers > 0 (readers hold it), 0 (free) or -1 (writer holds it,
 * writer set); writers_waiting counts sleepers in rwsem_down_write. New
 * readers wait while writers_waiting > 0, so writers are not starved.
 * Locks: s->guard (LOCK_LEVEL_OBJECT) protects the fields and is the
 * condition lock passed to wq_sleep. The RwSem itself is a sleeping lock at
 * s->level; debug builds check its order like a mutex's.
 * Cost: O(1) without contention; a release wakes every waiter (they re-check),
 * which is fine for the few threads that share one mm. */

#include "proc/rwsem.h"
#include "proc/sched.h"

void rwsem_init(RwSem *s, uint16_t level) {
    spin_lock_init(&s->guard, LOCK_LEVEL_OBJECT, "rwsem guard");
    s->readers = 0;
    s->writers_waiting = 0;
    s->writer = NULL;
    s->level = level;
    wq_init(&s->waiters);
}

void rwsem_down_read(RwSem *s) {
    lockdep_sleep_acquire(s->level, "rwsem read");
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    while (s->readers < 0 || s->writers_waiting > 0) {
        wq_sleep(&s->waiters, &s->guard, flags);
        spin_lock_irqsave(&s->guard, &flags);
    }
    s->readers++;
    spin_unlock_irqrestore(&s->guard, flags);
}

int rwsem_try_read(RwSem *s) {
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    if (s->readers < 0 || s->writers_waiting > 0) {
        spin_unlock_irqrestore(&s->guard, flags);
        return -1;
    }
    s->readers++;
    spin_unlock_irqrestore(&s->guard, flags);
    lockdep_sleep_acquire(s->level, "rwsem read");
    return 0;
}

void rwsem_up_read(RwSem *s) {
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    s->readers--;
    if (s->readers == 0) wq_wake_all(&s->waiters);   /* a writer may be waiting */
    spin_unlock_irqrestore(&s->guard, flags);
    lockdep_sleep_release(s->level);
}

void rwsem_down_write(RwSem *s) {
    lockdep_sleep_acquire(s->level, "rwsem write");
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    s->writers_waiting++;
    while (s->readers != 0) {
        wq_sleep(&s->waiters, &s->guard, flags);
        spin_lock_irqsave(&s->guard, &flags);
    }
    s->writers_waiting--;
    s->readers = -1;
    s->writer = thread_current();
    spin_unlock_irqrestore(&s->guard, flags);
}

void rwsem_up_write(RwSem *s) {
    uint64_t flags;
    spin_lock_irqsave(&s->guard, &flags);
    s->readers = 0;
    s->writer = NULL;
    wq_wake_all(&s->waiters);
    spin_unlock_irqrestore(&s->guard, flags);
    lockdep_sleep_release(s->level);
}

int rwsem_write_held(const RwSem *s) {
    return s->readers < 0 && s->writer == thread_current();
}
