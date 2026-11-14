#ifndef ARCHOS_PROC_RWSEM_H
#define ARCHOS_PROC_RWSEM_H

#include "proc/spinlock.h"
#include "proc/waitqueue.h"
#include "proc/thread.h"

/* Sleeping read-write lock (M1-07; the mm lock, work-blocks.md §7 level 2).
 *
 * Many readers or one writer. A waiting writer stops new readers, so a stream
 * of readers (page faults) can't starve a writer (munmap). Like Mutex: thread
 * context only, never from an interrupt handler, never while holding a
 * spinlock (debug builds check it), and not recursive: a thread that holds it
 * for reading and takes it again can deadlock behind a waiting writer. */
typedef struct RwSem {
    Spinlock guard;              /* protects the fields below; condition lock for wq_sleep */
    int32_t readers;             /* > 0: that many readers; -1: a writer holds it */
    uint32_t writers_waiting;    /* writers sleeping in rwsem_down_write */
    Thread *writer;              /* the writer, or NULL */
    WaitQueue waiters;
    uint16_t level;              /* sleeping-lock level (LOCK_LEVEL_*, lockdep.h) */
    uint16_t write_level;        /* level the writer took it at (rwsem_down_write_nested) */
} RwSem;

/* Initialize with its sleeping-lock level. */
void rwsem_init(RwSem *s, uint16_t level);

/* Take for reading; sleeps while a writer holds it or waits for it. */
void rwsem_down_read(RwSem *s);

/* Take for reading without sleeping. Returns 0, or -1 if a writer holds it
 * or waits for it. */
int rwsem_try_read(RwSem *s);

/* Release a read hold. */
void rwsem_up_read(RwSem *s);

/* Take for writing; sleeps until no reader or writer holds it. */
void rwsem_down_write(RwSem *s);

/* Take for writing, checked by the lock-order checker as a lock of `level`
 * instead of s->level: the second of two same-kind locks held together
 * (rename's second directory, LOCK_LEVEL_INODE_2). */
void rwsem_down_write_nested(RwSem *s, uint16_t level);

/* Release the write hold. */
void rwsem_up_write(RwSem *s);

/* 1 if the running thread holds it for writing. */
int rwsem_write_held(const RwSem *s);

#endif /* ARCHOS_PROC_RWSEM_H */
