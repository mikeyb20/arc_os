/* arc_os — Host-side tests for mutex, semaphore, and condvar */

#define _DEFAULT_SOURCE

#include "test_framework.h"
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

/* Guard kernel headers */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_STRING_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_PROC_SPINLOCK_H
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_WAITQUEUE_H
#define ARCHOS_PROC_MUTEX_H
#define ARCHOS_PROC_SEMAPHORE_H
#define ARCHOS_PROC_CONDVAR_H

static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* ---- Spinlock stub (uses pthreads) ---- */

typedef struct {
    volatile uint32_t locked;
    uint64_t saved_flags;
} Spinlock;

#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }

static inline void spinlock_acquire(Spinlock *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        /* spin */
    }
    lock->saved_flags = 0;
}

static inline void spinlock_release(Spinlock *lock) {
    __sync_lock_release(&lock->locked);
}

/* ---- Thread stub (uses pthreads) ---- */

#define THREAD_CREATED  0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_DEAD     4

typedef struct Thread {
    uint32_t      tid;
    uint8_t       state;
    struct Thread *next;
} Thread;

/* Per-pthread thread identity */
static __thread Thread tls_thread;
static uint32_t next_tid = 1;

static Thread *thread_current(void) {
    return &tls_thread;
}

/* ---- Wait queue stub (uses condvar/mutex from pthreads) ---- */

typedef struct WaitQueue {
    Spinlock lock;
    Thread  *head;
    Thread  *tail;
    /* pthreads backing */
    pthread_mutex_t pmtx;
    pthread_cond_t  pcond;
} WaitQueue;

#define WAITQUEUE_INIT { \
    .lock = SPINLOCK_INIT, .head = NULL, .tail = NULL, \
    .pmtx = PTHREAD_MUTEX_INITIALIZER, \
    .pcond = PTHREAD_COND_INITIALIZER \
}

static void wq_init(WaitQueue *wq) {
    wq->lock = (Spinlock)SPINLOCK_INIT;
    wq->head = NULL;
    wq->tail = NULL;
    pthread_mutex_init(&wq->pmtx, NULL);
    pthread_cond_init(&wq->pcond, NULL);
}

/* sched stubs — actual blocking done by pthreads in wq_sleep */
static void stub_sched_yield(void) __attribute__((unused));
static void stub_sched_yield(void) { }

static void sched_add_thread(Thread *t) __attribute__((unused));
static void sched_add_thread(Thread *t) { t->state = THREAD_READY; }

static void wq_sleep(WaitQueue *wq, Spinlock *lock) {
    Thread *self = thread_current();
    self->state = THREAD_BLOCKED;
    spinlock_release(lock);
    pthread_mutex_lock(&wq->pmtx);
    pthread_cond_wait(&wq->pcond, &wq->pmtx);
    pthread_mutex_unlock(&wq->pmtx);
    self->state = THREAD_RUNNING;
}

static int wq_wake(WaitQueue *wq) {
    pthread_mutex_lock(&wq->pmtx);
    pthread_cond_signal(&wq->pcond);
    pthread_mutex_unlock(&wq->pmtx);
    return 1;
}

static int wq_wake_all(WaitQueue *wq) {
    pthread_mutex_lock(&wq->pmtx);
    pthread_cond_broadcast(&wq->pcond);
    pthread_mutex_unlock(&wq->pmtx);
    return 1;
}

/* ---- Now include the implementations ---- */

/* Mutex */
typedef struct {
    Spinlock  guard;
    Thread   *owner;
    WaitQueue waiters;
} Mutex;

#define MUTEX_INIT { \
    .guard = SPINLOCK_INIT, \
    .owner = NULL, \
    .waiters = WAITQUEUE_INIT \
}

static void mutex_init(Mutex *m) {
    m->guard = (Spinlock)SPINLOCK_INIT;
    m->owner = NULL;
    wq_init(&m->waiters);
}

static void mutex_lock(Mutex *m) {
    spinlock_acquire(&m->guard);
    while (m->owner != NULL) {
        wq_sleep(&m->waiters, &m->guard);
        spinlock_acquire(&m->guard);
    }
    m->owner = thread_current();
    spinlock_release(&m->guard);
}

static void mutex_unlock(Mutex *m) {
    spinlock_acquire(&m->guard);
    m->owner = NULL;
    wq_wake(&m->waiters);
    spinlock_release(&m->guard);
}

static int mutex_trylock(Mutex *m) {
    spinlock_acquire(&m->guard);
    if (m->owner != NULL) {
        spinlock_release(&m->guard);
        return -1;
    }
    m->owner = thread_current();
    spinlock_release(&m->guard);
    return 0;
}

/* Semaphore */
typedef struct {
    Spinlock  guard;
    int32_t   count;
    WaitQueue waiters;
} Semaphore;

static void sem_init(Semaphore *s, int32_t initial_count) {
    s->guard = (Spinlock)SPINLOCK_INIT;
    s->count = initial_count;
    wq_init(&s->waiters);
}

static void sem_wait(Semaphore *s) {
    spinlock_acquire(&s->guard);
    while (s->count <= 0) {
        wq_sleep(&s->waiters, &s->guard);
        spinlock_acquire(&s->guard);
    }
    s->count--;
    spinlock_release(&s->guard);
}

static void sem_post(Semaphore *s) {
    spinlock_acquire(&s->guard);
    s->count++;
    wq_wake(&s->waiters);
    spinlock_release(&s->guard);
}

static int sem_trywait(Semaphore *s) {
    spinlock_acquire(&s->guard);
    if (s->count <= 0) {
        spinlock_release(&s->guard);
        return -1;
    }
    s->count--;
    spinlock_release(&s->guard);
    return 0;
}

static int32_t sem_getvalue(Semaphore *s) {
    spinlock_acquire(&s->guard);
    int32_t val = s->count;
    spinlock_release(&s->guard);
    return val;
}

/* Condvar */
typedef struct {
    WaitQueue waiters;
} CondVar;

static void condvar_init(CondVar *cv) {
    wq_init(&cv->waiters);
}

static void condvar_wait(CondVar *cv, Mutex *m) {
    spinlock_acquire(&cv->waiters.lock);
    spinlock_release(&cv->waiters.lock);
    mutex_unlock(m);
    /* Use pthreads to actually block */
    pthread_mutex_lock(&cv->waiters.pmtx);
    pthread_cond_wait(&cv->waiters.pcond, &cv->waiters.pmtx);
    pthread_mutex_unlock(&cv->waiters.pmtx);
    mutex_lock(m);
}

static void condvar_signal(CondVar *cv) {
    wq_wake(&cv->waiters);
}

static void condvar_broadcast(CondVar *cv) {
    wq_wake_all(&cv->waiters);
}

/* ==================================================================
 * Tests
 * ================================================================== */

/* --- Mutex tests --- */

TEST(mutex_init_unlocked) {
    Mutex m;
    mutex_init(&m);
    ASSERT_TRUE(m.owner == NULL);
    return 0;
}

TEST(mutex_lock_unlock) {
    tls_thread.tid = next_tid++;
    Mutex m;
    mutex_init(&m);
    mutex_lock(&m);
    ASSERT_TRUE(m.owner == thread_current());
    mutex_unlock(&m);
    ASSERT_TRUE(m.owner == NULL);
    return 0;
}

TEST(mutex_trylock_success) {
    tls_thread.tid = next_tid++;
    Mutex m;
    mutex_init(&m);
    ASSERT_EQ(mutex_trylock(&m), 0);
    ASSERT_TRUE(m.owner == thread_current());
    mutex_unlock(&m);
    return 0;
}

TEST(mutex_trylock_fail) {
    tls_thread.tid = next_tid++;
    Mutex m;
    mutex_init(&m);
    mutex_lock(&m);
    /* Simulate another thread trying */
    Thread fake = { .tid = 99 };
    Thread *saved = m.owner;
    /* trylock should fail because owner is set */
    ASSERT_EQ(mutex_trylock(&m), -1);
    ASSERT_TRUE(m.owner == saved);
    mutex_unlock(&m);
    (void)fake;
    return 0;
}

/* Threaded mutex test */
static Mutex contended_mutex;
static volatile int shared_counter;

static void *mutex_incrementer(void *arg) {
    tls_thread.tid = next_tid++;
    int count = *(int *)arg;
    for (int i = 0; i < count; i++) {
        mutex_lock(&contended_mutex);
        shared_counter++;
        mutex_unlock(&contended_mutex);
    }
    return NULL;
}

TEST(mutex_contended_correctness) {
    mutex_init(&contended_mutex);
    shared_counter = 0;
    int iters = 1000;

    pthread_t t1, t2;
    pthread_create(&t1, NULL, mutex_incrementer, &iters);
    pthread_create(&t2, NULL, mutex_incrementer, &iters);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT_EQ(shared_counter, 2000);
    return 0;
}

/* --- Semaphore tests --- */

TEST(sem_init_value) {
    Semaphore s;
    sem_init(&s, 5);
    ASSERT_EQ(sem_getvalue(&s), 5);
    return 0;
}

TEST(sem_wait_decrements) {
    tls_thread.tid = next_tid++;
    Semaphore s;
    sem_init(&s, 3);
    sem_wait(&s);
    ASSERT_EQ(sem_getvalue(&s), 2);
    sem_wait(&s);
    ASSERT_EQ(sem_getvalue(&s), 1);
    return 0;
}

TEST(sem_post_increments) {
    Semaphore s;
    sem_init(&s, 0);
    sem_post(&s);
    ASSERT_EQ(sem_getvalue(&s), 1);
    sem_post(&s);
    ASSERT_EQ(sem_getvalue(&s), 2);
    return 0;
}

TEST(sem_trywait_success) {
    tls_thread.tid = next_tid++;
    Semaphore s;
    sem_init(&s, 1);
    ASSERT_EQ(sem_trywait(&s), 0);
    ASSERT_EQ(sem_getvalue(&s), 0);
    return 0;
}

TEST(sem_trywait_fail) {
    Semaphore s;
    sem_init(&s, 0);
    ASSERT_EQ(sem_trywait(&s), -1);
    ASSERT_EQ(sem_getvalue(&s), 0);
    return 0;
}

TEST(sem_binary_mutex) {
    /* Use semaphore(1) as a binary lock */
    tls_thread.tid = next_tid++;
    Semaphore s;
    sem_init(&s, 1);
    sem_wait(&s);
    ASSERT_EQ(sem_getvalue(&s), 0);
    ASSERT_EQ(sem_trywait(&s), -1);
    sem_post(&s);
    ASSERT_EQ(sem_getvalue(&s), 1);
    return 0;
}

/* Threaded semaphore producer-consumer test */
static Semaphore prod_sem;
static volatile int produced_count;

static void *sem_producer(void *arg) {
    tls_thread.tid = next_tid++;
    int count = *(int *)arg;
    for (int i = 0; i < count; i++) {
        __sync_fetch_and_add(&produced_count, 1);
        sem_post(&prod_sem);
    }
    return NULL;
}

static void *sem_consumer(void *arg) {
    tls_thread.tid = next_tid++;
    int count = *(int *)arg;
    for (int i = 0; i < count; i++) {
        sem_wait(&prod_sem);
    }
    return NULL;
}

TEST(sem_producer_consumer) {
    sem_init(&prod_sem, 0);
    produced_count = 0;
    int count = 500;

    pthread_t p, c;
    pthread_create(&p, NULL, sem_producer, &count);
    pthread_create(&c, NULL, sem_consumer, &count);
    pthread_join(p, NULL);
    pthread_join(c, NULL);

    ASSERT_EQ(produced_count, 500);
    ASSERT_EQ(sem_getvalue(&prod_sem), 0);
    return 0;
}

/* --- Condvar tests --- */

TEST(condvar_init_empty) {
    CondVar cv;
    condvar_init(&cv);
    /* Just verify it doesn't crash */
    return 0;
}

/* Threaded condvar test */
static Mutex cv_mutex;
static CondVar cv_cond;
static volatile int cv_ready;

static void *cv_waiter(void *arg) {
    (void)arg;
    tls_thread.tid = next_tid++;
    mutex_lock(&cv_mutex);
    while (!cv_ready) {
        condvar_wait(&cv_cond, &cv_mutex);
    }
    mutex_unlock(&cv_mutex);
    return NULL;
}

TEST(condvar_signal_wakes_one) {
    mutex_init(&cv_mutex);
    condvar_init(&cv_cond);
    cv_ready = 0;
    tls_thread.tid = next_tid++;

    pthread_t w;
    pthread_create(&w, NULL, cv_waiter, NULL);

    /* Give waiter time to block */
    usleep(10000);

    mutex_lock(&cv_mutex);
    cv_ready = 1;
    condvar_signal(&cv_cond);
    mutex_unlock(&cv_mutex);

    pthread_join(w, NULL);
    ASSERT_EQ(cv_ready, 1);
    return 0;
}

static volatile int cv_broadcast_count;

static void *cv_broadcast_waiter(void *arg) {
    (void)arg;
    tls_thread.tid = next_tid++;
    mutex_lock(&cv_mutex);
    while (!cv_ready) {
        condvar_wait(&cv_cond, &cv_mutex);
    }
    __sync_fetch_and_add(&cv_broadcast_count, 1);
    mutex_unlock(&cv_mutex);
    return NULL;
}

TEST(condvar_broadcast_wakes_all) {
    mutex_init(&cv_mutex);
    condvar_init(&cv_cond);
    cv_ready = 0;
    cv_broadcast_count = 0;
    tls_thread.tid = next_tid++;

    pthread_t w1, w2, w3;
    pthread_create(&w1, NULL, cv_broadcast_waiter, NULL);
    pthread_create(&w2, NULL, cv_broadcast_waiter, NULL);
    pthread_create(&w3, NULL, cv_broadcast_waiter, NULL);

    usleep(10000);

    mutex_lock(&cv_mutex);
    cv_ready = 1;
    condvar_broadcast(&cv_cond);
    mutex_unlock(&cv_mutex);

    pthread_join(w1, NULL);
    pthread_join(w2, NULL);
    pthread_join(w3, NULL);

    ASSERT_EQ(cv_broadcast_count, 3);
    return 0;
}

/* --- Suite --- */

TestCase mutex_tests[] = {
    TEST_ENTRY(mutex_init_unlocked),
    TEST_ENTRY(mutex_lock_unlock),
    TEST_ENTRY(mutex_trylock_success),
    TEST_ENTRY(mutex_trylock_fail),
    TEST_ENTRY(mutex_contended_correctness),
    TEST_ENTRY(sem_init_value),
    TEST_ENTRY(sem_wait_decrements),
    TEST_ENTRY(sem_post_increments),
    TEST_ENTRY(sem_trywait_success),
    TEST_ENTRY(sem_trywait_fail),
    TEST_ENTRY(sem_binary_mutex),
    TEST_ENTRY(sem_producer_consumer),
    TEST_ENTRY(condvar_init_empty),
    TEST_ENTRY(condvar_signal_wakes_one),
    TEST_ENTRY(condvar_broadcast_wakes_all),
};
int mutex_test_count = sizeof(mutex_tests) / sizeof(mutex_tests[0]);
