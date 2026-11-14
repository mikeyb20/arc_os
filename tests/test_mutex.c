/* arc_os — Host-side tests for mutex, semaphore, and condvar */

#define _DEFAULT_SOURCE

#include "test_framework.h"
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

/* The real mutex.c, semaphore.c, condvar.c and waitqueue.c run here, on host
 * threads. Each host thread is one kernel thread on its own simulated CPU
 * (arch/hal_lock.h); the scheduler shim below blocks a BLOCKED thread on a
 * pthread condition until a wakeup makes it runnable, as sched.c would. The
 * names are prefixed so they don't meet test_sched.c's real scheduler or
 * test_waitqueue.c's copy of waitqueue.c at link time. */
#define ARCHOS_LIB_KPRINTF_H
#define thread_current     mutex_test_thread_current
#define thread_set_current mutex_test_thread_set_current
#define context_switch     mutex_test_context_switch
#define sched_schedule     mutex_test_sched_schedule
#define sched_wake         mutex_test_sched_wake
#define sched_yield        mutex_test_sched_yield   /* <pthread.h> declares the POSIX one */
#define wq_init            mutex_test_wq_init
#define wq_sleep           mutex_test_wq_sleep
#define wq_wake            mutex_test_wq_wake
#define wq_wake_all        mutex_test_wq_wake_all

static inline void kprintf(const char *fmt, ...) { (void)fmt; }

#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/spinlock.h"

static _Thread_local Thread tls_thread;
static uint32_t next_tid = 1;

Thread *mutex_test_thread_current(void) {
    return &tls_thread;
}

/* Scheduler shim: blocking and waking under one host mutex, so a wakeup that
 * comes before the sleeper reaches its wait is never lost */
static pthread_mutex_t shim_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shim_cond = PTHREAD_COND_INITIALIZER;
static int shim_sleeps;

void mutex_test_sched_schedule(void) {
    Thread *self = &tls_thread;
    pthread_mutex_lock(&shim_lock);
    if (self->state == THREAD_BLOCKED) shim_sleeps++;
    while (self->state == THREAD_BLOCKED) pthread_cond_wait(&shim_cond, &shim_lock);
    self->state = THREAD_RUNNING;
    pthread_mutex_unlock(&shim_lock);
}

int mutex_test_sched_wake(Thread *t) {
    pthread_mutex_lock(&shim_lock);
    int woke = t->state == THREAD_BLOCKED;
    if (woke) {
        t->state = THREAD_READY;
        pthread_cond_broadcast(&shim_cond);
    }
    pthread_mutex_unlock(&shim_lock);
    return woke;
}

#include "../kernel/proc/waitqueue.c"
#include "../kernel/proc/mutex.c"
#include "../kernel/proc/semaphore.c"
#include "../kernel/proc/condvar.c"
#include "../kernel/proc/rwsem.c"

/* Every host thread starts as a running kernel thread with its own id */
static void thread_start(void) {
    tls_thread.tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    tls_thread.state = THREAD_RUNNING;
}

/* ==================================================================
 * Tests
 * ================================================================== */

/* --- Mutex tests --- */

TEST(mutex_init_unlocked) {
    Mutex m;
    mutex_init(&m, LOCK_LEVEL_INODE);
    ASSERT_TRUE(m.owner == NULL);
    return 0;
}

TEST(mutex_lock_unlock) {
    thread_start();
    Mutex m;
    mutex_init(&m, LOCK_LEVEL_INODE);
    mutex_lock(&m);
    ASSERT_TRUE(m.owner == thread_current());
    mutex_unlock(&m);
    ASSERT_TRUE(m.owner == NULL);
    return 0;
}

TEST(mutex_trylock_success) {
    thread_start();
    Mutex m;
    mutex_init(&m, LOCK_LEVEL_INODE);
    ASSERT_EQ(mutex_trylock(&m), 0);
    ASSERT_TRUE(m.owner == thread_current());
    mutex_unlock(&m);
    return 0;
}

TEST(mutex_trylock_fail) {
    thread_start();
    Mutex m;
    mutex_init(&m, LOCK_LEVEL_INODE);
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
    thread_start();
    int count = *(int *)arg;
    for (int i = 0; i < count; i++) {
        mutex_lock(&contended_mutex);
        shared_counter++;
        mutex_unlock(&contended_mutex);
    }
    return NULL;
}

/* Four host threads (four CPUs) contend: waiters really sleep and are woken */
TEST(mutex_contended_correctness) {
    mutex_init(&contended_mutex, LOCK_LEVEL_INODE);
    shared_counter = 0;
    int iters = 20000;

    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, mutex_incrementer, &iters);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);

    ASSERT_EQ(shared_counter, 80000);
    ASSERT_TRUE(contended_mutex.owner == NULL);
    ASSERT_TRUE(contended_mutex.waiters.head == NULL);
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
    thread_start();
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
    thread_start();
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
    thread_start();
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
    thread_start();
    int count = *(int *)arg;
    for (int i = 0; i < count; i++) {
        __sync_fetch_and_add(&produced_count, 1);
        sem_post(&prod_sem);
    }
    return NULL;
}

static void *sem_consumer(void *arg) {
    thread_start();
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
    thread_start();
    mutex_lock(&cv_mutex);
    while (!cv_ready) {
        condvar_wait(&cv_cond, &cv_mutex);
    }
    mutex_unlock(&cv_mutex);
    return NULL;
}

TEST(condvar_signal_wakes_one) {
    mutex_init(&cv_mutex, LOCK_LEVEL_INODE);
    condvar_init(&cv_cond);
    cv_ready = 0;
    thread_start();

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
    thread_start();
    mutex_lock(&cv_mutex);
    while (!cv_ready) {
        condvar_wait(&cv_cond, &cv_mutex);
    }
    __sync_fetch_and_add(&cv_broadcast_count, 1);
    mutex_unlock(&cv_mutex);
    return NULL;
}

TEST(condvar_broadcast_wakes_all) {
    mutex_init(&cv_mutex, LOCK_LEVEL_INODE);
    condvar_init(&cv_cond);
    cv_ready = 0;
    cv_broadcast_count = 0;
    thread_start();

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

/* --- Read-write lock tests (the mm lock, M1-07) --- */

TEST(rwsem_readers_share) {
    thread_start();
    RwSem s;
    rwsem_init(&s, LOCK_LEVEL_MM);
    rwsem_down_read(&s);
    ASSERT_EQ(rwsem_try_read(&s), 0);   /* a second reader gets in */
    ASSERT_EQ(s.readers, 2);
    ASSERT_EQ(rwsem_write_held(&s), 0);
    rwsem_up_read(&s);
    rwsem_up_read(&s);
    ASSERT_EQ(s.readers, 0);
    return 0;
}

TEST(rwsem_writer_excludes_readers) {
    thread_start();
    RwSem s;
    rwsem_init(&s, LOCK_LEVEL_MM);
    rwsem_down_write(&s);
    ASSERT_EQ(rwsem_write_held(&s), 1);
    ASSERT_EQ(s.readers, -1);
    ASSERT_EQ(rwsem_try_read(&s), -1);
    rwsem_up_write(&s);
    ASSERT_EQ(rwsem_write_held(&s), 0);
    ASSERT_EQ(rwsem_try_read(&s), 0);
    rwsem_up_read(&s);
    return 0;
}

static RwSem rw_sem;
static volatile int rw_writer_done;
static void *rw_writer(void *arg) {
    (void)arg;
    thread_start();
    rwsem_down_write(&rw_sem);
    rw_writer_done = 1;
    rwsem_up_write(&rw_sem);
    return NULL;
}

/* A waiting writer keeps new readers out, so faults can't starve munmap */
TEST(rwsem_waiting_writer_blocks_new_readers) {
    thread_start();
    rwsem_init(&rw_sem, LOCK_LEVEL_MM);
    rw_writer_done = 0;
    rwsem_down_read(&rw_sem);
    pthread_t w;
    pthread_create(&w, NULL, rw_writer, NULL);
    for (int i = 0; i < 2000 && __atomic_load_n(&rw_sem.writers_waiting, __ATOMIC_ACQUIRE) == 0; i++) usleep(500);
    ASSERT_EQ(rw_sem.writers_waiting, 1);
    ASSERT_EQ(rwsem_try_read(&rw_sem), -1);
    ASSERT_EQ(rw_writer_done, 0);
    rwsem_up_read(&rw_sem);   /* the writer goes now */
    pthread_join(w, NULL);
    ASSERT_EQ(rw_writer_done, 1);
    ASSERT_EQ(rw_sem.readers, 0);
    ASSERT_TRUE(rw_sem.waiters.head == NULL);
    return 0;
}

/* Two writers keep a == b, two readers check it: readers never see a write
 * in progress, and no update is lost */
static volatile int rw_a, rw_b;
static volatile int rw_torn;
static void *rw_mixed_writer(void *arg) {
    int n = *(int *)arg;
    thread_start();
    for (int i = 0; i < n; i++) {
        rwsem_down_write(&rw_sem);
        rw_a++;
        for (volatile int d = 0; d < 20; d++) {
        }
        rw_b++;
        rwsem_up_write(&rw_sem);
    }
    return NULL;
}
static void *rw_mixed_reader(void *arg) {
    int n = *(int *)arg;
    thread_start();
    for (int i = 0; i < n; i++) {
        rwsem_down_read(&rw_sem);
        if (rw_a != rw_b) rw_torn = 1;
        rwsem_up_read(&rw_sem);
    }
    return NULL;
}

TEST(rwsem_contended_correctness) {
    rwsem_init(&rw_sem, LOCK_LEVEL_MM);
    rw_a = rw_b = 0;
    rw_torn = 0;
    int iters = 5000;
    pthread_t t[4];
    pthread_create(&t[0], NULL, rw_mixed_writer, &iters);
    pthread_create(&t[1], NULL, rw_mixed_reader, &iters);
    pthread_create(&t[2], NULL, rw_mixed_writer, &iters);
    pthread_create(&t[3], NULL, rw_mixed_reader, &iters);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    ASSERT_EQ(rw_a, 10000);
    ASSERT_EQ(rw_b, 10000);
    ASSERT_EQ(rw_torn, 0);
    ASSERT_EQ(rw_sem.readers, 0);
    ASSERT_EQ(rw_sem.writers_waiting, 0);
    ASSERT_TRUE(rw_sem.waiters.head == NULL);
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
    TEST_ENTRY(rwsem_readers_share),
    TEST_ENTRY(rwsem_writer_excludes_readers),
    TEST_ENTRY(rwsem_waiting_writer_blocks_new_readers),
    TEST_ENTRY(rwsem_contended_correctness),
};
int mutex_test_count = sizeof(mutex_tests) / sizeof(mutex_tests[0]);
