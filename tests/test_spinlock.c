/* arc_os — Host-side tests for kernel/proc/spinlock.h */

#include "test_framework.h"
#include <stdint.h>
#include <pthread.h>

/* We cannot include the real spinlock.h because it uses privileged asm
 * (pushf/popf/cli). Reproduce the struct and provide host-safe versions. */

typedef struct {
    volatile uint32_t locked;
    uint64_t saved_flags;
} Spinlock;

#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }

static inline void spinlock_acquire(Spinlock *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        /* spin */
    }
    lock->saved_flags = 0;  /* No real flags on host */
}

static inline void spinlock_release(Spinlock *lock) {
    __sync_lock_release(&lock->locked);
}

/* --- Tests --- */

TEST(init_state) {
    Spinlock lock = SPINLOCK_INIT;
    ASSERT_EQ(lock.locked, 0);
    ASSERT_EQ(lock.saved_flags, 0);
    return 0;
}

TEST(acquire_sets_locked) {
    Spinlock lock = SPINLOCK_INIT;
    spinlock_acquire(&lock);
    ASSERT_EQ(lock.locked, 1);
    spinlock_release(&lock);
    return 0;
}

TEST(release_clears_locked) {
    Spinlock lock = SPINLOCK_INIT;
    spinlock_acquire(&lock);
    spinlock_release(&lock);
    ASSERT_EQ(lock.locked, 0);
    return 0;
}

TEST(double_release) {
    Spinlock lock = SPINLOCK_INIT;
    spinlock_acquire(&lock);
    spinlock_release(&lock);
    spinlock_release(&lock);
    ASSERT_EQ(lock.locked, 0);
    return 0;
}

TEST(reacquire_after_release) {
    Spinlock lock = SPINLOCK_INIT;
    spinlock_acquire(&lock);
    spinlock_release(&lock);
    spinlock_acquire(&lock);
    ASSERT_EQ(lock.locked, 1);
    spinlock_release(&lock);
    ASSERT_EQ(lock.locked, 0);
    return 0;
}

TEST(multiple_locks_independent) {
    Spinlock a = SPINLOCK_INIT;
    Spinlock b = SPINLOCK_INIT;
    spinlock_acquire(&a);
    ASSERT_EQ(a.locked, 1);
    ASSERT_EQ(b.locked, 0);
    spinlock_acquire(&b);
    ASSERT_EQ(a.locked, 1);
    ASSERT_EQ(b.locked, 1);
    spinlock_release(&a);
    ASSERT_EQ(a.locked, 0);
    ASSERT_EQ(b.locked, 1);
    spinlock_release(&b);
    return 0;
}

/* Contention test: 2 pthreads × 100K increments == 200K */
static Spinlock contention_lock = SPINLOCK_INIT;
static volatile int contention_counter = 0;

static void *contention_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        spinlock_acquire(&contention_lock);
        contention_counter++;
        spinlock_release(&contention_lock);
    }
    return NULL;
}

TEST(contention) {
    contention_lock = (Spinlock)SPINLOCK_INIT;
    contention_counter = 0;

    pthread_t t1, t2;
    pthread_create(&t1, NULL, contention_worker, NULL);
    pthread_create(&t2, NULL, contention_worker, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT_EQ(contention_counter, 200000);
    return 0;
}

TEST(struct_size) {
    ASSERT_EQ(sizeof(Spinlock), 16);
    return 0;
}

TEST(acquire_release_cycle) {
    Spinlock lock = SPINLOCK_INIT;
    for (int i = 0; i < 10000; i++) {
        spinlock_acquire(&lock);
        spinlock_release(&lock);
    }
    ASSERT_EQ(lock.locked, 0);
    return 0;
}

/* --- Test suite export --- */

TestCase spinlock_tests[] = {
    TEST_ENTRY(init_state),
    TEST_ENTRY(acquire_sets_locked),
    TEST_ENTRY(release_clears_locked),
    TEST_ENTRY(double_release),
    TEST_ENTRY(reacquire_after_release),
    TEST_ENTRY(multiple_locks_independent),
    TEST_ENTRY(contention),
    TEST_ENTRY(struct_size),
    TEST_ENTRY(acquire_release_cycle),
};

int spinlock_test_count = sizeof(spinlock_tests) / sizeof(spinlock_tests[0]);
