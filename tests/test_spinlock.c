/* arc_os — Host-side tests for kernel/proc/spinlock.h and the lock-order rule
 * (kernel/proc/lockdep.c). The real header runs here: arch/hal_lock.h gives
 * host builds a simulated interrupt flag and spinlock depth, one per host
 * thread (each host thread plays one CPU). */

#include "test_framework.h"
#include <stdint.h>
#include <pthread.h>

#define ARCHOS_LIB_KPRINTF_H
static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}

#include "proc/spinlock.h"
#include "../kernel/proc/lockdep.c"

/* The simulated CPU state every host test shares (hal_lock.h) */
_Thread_local int hal_test_irq_enabled = 1;
_Thread_local uint32_t hal_test_lock_depth;

static void reset_cpu(void) {
    hal_test_irq_enabled = 1;
    hal_test_lock_depth = 0;
}

/* --- Spinlock --- */

TEST(init_state) {
    Spinlock lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "obj");
    ASSERT_EQ(lock.locked, 0);
    ASSERT_EQ(lock.level, LOCK_LEVEL_OBJECT);
    ASSERT_EQ(lock.subkey, 0);
    ASSERT_STR_EQ(lock.name, "obj");
    Spinlock rt;
    spin_lock_init(&rt, LOCK_LEVEL_BUDDY, "buddy");
    ASSERT_EQ(rt.locked, 0);
    ASSERT_EQ(rt.level, LOCK_LEVEL_BUDDY);
    ASSERT_STR_EQ(rt.name, "buddy");
    return 0;
}

TEST(lock_unlock_tracks_depth) {
    reset_cpu();
    Spinlock lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "obj");
    spin_lock(&lock);
    ASSERT_EQ(lock.locked, 1);
    ASSERT_EQ(hal_lock_depth(), 1);
    ASSERT_EQ(hal_test_irq_enabled, 1);   /* plain spin_lock leaves the flag alone */
    spin_unlock(&lock);
    ASSERT_EQ(lock.locked, 0);
    ASSERT_EQ(hal_lock_depth(), 0);
    return 0;
}

TEST(trylock) {
    reset_cpu();
    Spinlock lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "obj");
    ASSERT_EQ(spin_trylock(&lock), 1);
    ASSERT_EQ(hal_lock_depth(), 1);
    ASSERT_EQ(spin_trylock(&lock), 0);    /* held: fails, depth unchanged */
    ASSERT_EQ(hal_lock_depth(), 1);
    ASSERT_TRUE(spin_is_locked(&lock));
    spin_unlock(&lock);
    ASSERT_FALSE(spin_is_locked(&lock));
    ASSERT_EQ(hal_lock_depth(), 0);
    return 0;
}

TEST(irqsave_restores_flag) {
    reset_cpu();
    Spinlock lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "obj");
    uint64_t flags;
    spin_lock_irqsave(&lock, &flags);
    ASSERT_EQ(hal_test_irq_enabled, 0);
    spin_unlock_irqrestore(&lock, flags);
    ASSERT_EQ(hal_test_irq_enabled, 1);

    hal_test_irq_enabled = 0;             /* already off: stays off */
    spin_lock_irqsave(&lock, &flags);
    spin_unlock_irqrestore(&lock, flags);
    ASSERT_EQ(hal_test_irq_enabled, 0);
    return 0;
}

/* The bug caller-held flags fix (M1-05 spec, work-blocks §7): take A then B,
 * release A then B, and interrupts stay off until the last release. When the
 * lock kept the flags, releasing A restored A's "on" while B was still held.
 * Now the caller hands each release the flags it wants: the inner ones (off)
 * to the first release, the outermost to the last. */
TEST(release_out_of_order_keeps_irqs_off) {
    reset_cpu();
    Spinlock a = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "a");
    Spinlock b = SPINLOCK_INIT(LOCK_LEVEL_WAITQUEUE, "b");
    uint64_t fa, fb;
    spin_lock_irqsave(&a, &fa);
    spin_lock_irqsave(&b, &fb);
    ASSERT_EQ(hal_lock_depth(), 2);
    spin_unlock_irqrestore(&a, fb);
    ASSERT_EQ(a.locked, 0);
    ASSERT_EQ(b.locked, 1);
    ASSERT_EQ(hal_test_irq_enabled, 0);   /* B still held: still off */
    ASSERT_EQ(hal_lock_depth(), 1);
    spin_unlock_irqrestore(&b, fa);
    ASSERT_EQ(hal_test_irq_enabled, 1);   /* on again only now */
    ASSERT_EQ(hal_lock_depth(), 0);
    return 0;
}

TEST(struct_size) {
    ASSERT_EQ(sizeof(Spinlock), 16);
    return 0;
}

/* Contention: 4 host threads (4 simulated CPUs) x 100K increments */
static Spinlock contention_lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "contended");
static volatile int contention_counter = 0;
static volatile int contention_bad_depth = 0;

static void *contention_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        uint64_t flags;
        spin_lock_irqsave(&contention_lock, &flags);
        contention_counter++;
        if (hal_lock_depth() != 1) contention_bad_depth = 1;
        spin_unlock_irqrestore(&contention_lock, flags);
    }
    if (hal_lock_depth() != 0 || hal_test_irq_enabled != 1) contention_bad_depth = 1;
    return NULL;
}

TEST(contention) {
    contention_counter = 0;
    contention_bad_depth = 0;
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, contention_worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    ASSERT_EQ(contention_counter, 400000);
    ASSERT_EQ(contention_bad_depth, 0);
    return 0;
}

/* --- Lock order (lockdep_check_order) --- */

static LockdepCpu held_cpu(const Spinlock *const *locks, uint32_t n) {
    LockdepCpu c;
    memset(&c, 0, sizeof(c));
    for (uint32_t i = 0; i < n; i++) c.held[i] = locks[i];
    c.depth = n;
    return c;
}

TEST(order_nothing_held) {
    ASSERT_TRUE(lockdep_check_order(NULL, NULL, LOCK_LEVEL_KERNEL, 0) == NULL);
    LockdepCpu c = held_cpu(NULL, 0);
    LockdepThread t = {{0}, 0};
    ASSERT_TRUE(lockdep_check_order(&c, &t, LOCK_LEVEL_PAGE_TABLE, 0) == NULL);
    return 0;
}

TEST(order_increasing_ok) {
    static const Spinlock pt = SPINLOCK_INIT(LOCK_LEVEL_PAGE_TABLE, "pt");
    static const Spinlock wq = SPINLOCK_INIT(LOCK_LEVEL_WAITQUEUE, "wq");
    const Spinlock *h[] = {&pt, &wq};
    LockdepCpu c = held_cpu(h, 2);
    ASSERT_TRUE(lockdep_check_order(&c, NULL, LOCK_LEVEL_RUNQUEUE, 0) == NULL);
    ASSERT_TRUE(lockdep_check_order(&c, NULL, LOCK_LEVEL_LOG, 0) == NULL);
    return 0;
}

/* The pair lockdep_test_inversion takes (suite "lockorder") */
TEST(order_inverted_pair_caught) {
    static const Spinlock wq = SPINLOCK_INIT(LOCK_LEVEL_WAITQUEUE, "wq");
    const Spinlock *h[] = {&wq};
    LockdepCpu c = held_cpu(h, 1);
    const char *why = lockdep_check_order(&c, NULL, LOCK_LEVEL_PAGE_TABLE, 0);
    ASSERT_TRUE(why != NULL);
    ASSERT_STR_EQ(why, "lower level than a lock held");
    return 0;
}

/* The highest held lock decides, wherever it sits in the list */
TEST(order_checks_every_held_lock) {
    static const Spinlock rq = SPINLOCK_INIT(LOCK_LEVEL_RUNQUEUE, "rq");
    static const Spinlock pt = SPINLOCK_INIT(LOCK_LEVEL_PAGE_TABLE, "pt");
    const Spinlock *h[] = {&rq, &pt};   /* already out of order: rq first */
    LockdepCpu c = held_cpu(h, 2);
    ASSERT_TRUE(lockdep_check_order(&c, NULL, LOCK_LEVEL_WAITQUEUE, 0) != NULL);
    return 0;
}

TEST(order_same_level_needs_subkeys) {
    static const Spinlock obj = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "obj");
    const Spinlock *h[] = {&obj};
    LockdepCpu c = held_cpu(h, 1);
    const char *why = lockdep_check_order(&c, NULL, LOCK_LEVEL_OBJECT, 0);
    ASSERT_TRUE(why != NULL);
    ASSERT_STR_EQ(why, "same level, not in subkey order");
    return 0;
}

/* Two run queues: only in CPU-index order (subkey = index + 1) */
TEST(order_run_queues_by_cpu) {
    Spinlock rq0 = SPINLOCK_INIT(LOCK_LEVEL_RUNQUEUE, "rq0");
    Spinlock rq3 = SPINLOCK_INIT(LOCK_LEVEL_RUNQUEUE, "rq3");
    rq0.subkey = 1;
    rq3.subkey = 4;
    const Spinlock *lo[] = {&rq0};
    LockdepCpu c = held_cpu(lo, 1);
    ASSERT_TRUE(lockdep_check_order(&c, NULL, LOCK_LEVEL_RUNQUEUE, rq3.subkey) == NULL);
    const Spinlock *hi[] = {&rq3};
    c = held_cpu(hi, 1);
    ASSERT_TRUE(lockdep_check_order(&c, NULL, LOCK_LEVEL_RUNQUEUE, rq0.subkey) != NULL);
    ASSERT_TRUE(lockdep_check_order(&c, NULL, LOCK_LEVEL_RUNQUEUE, rq3.subkey) != NULL);   /* itself */
    return 0;
}

/* Sleeping locks the thread holds count too: no inode lock (10) under the mm
 * lock (20), and spinlocks above it are fine */
TEST(order_sleeping_locks) {
    LockdepThread t = {{LOCK_LEVEL_MM}, 1};
    ASSERT_TRUE(lockdep_check_order(NULL, &t, LOCK_LEVEL_INODE, 0) != NULL);
    ASSERT_TRUE(lockdep_check_order(NULL, &t, LOCK_LEVEL_PAGE, 0) == NULL);
    ASSERT_TRUE(lockdep_check_order(NULL, &t, LOCK_LEVEL_PAGE_TABLE, 0) == NULL);
    return 0;
}

/* The kernel lock is level 0: nothing may be held when it is taken */
TEST(order_kernel_lock_first) {
    static const Spinlock wq = SPINLOCK_INIT(LOCK_LEVEL_WAITQUEUE, "wq");
    const Spinlock *h[] = {&wq};
    LockdepCpu c = held_cpu(h, 1);
    ASSERT_TRUE(lockdep_check_order(&c, NULL, LOCK_LEVEL_KERNEL, 0) != NULL);
    return 0;
}

/* Taking the kernel lock back after a sleep may happen under a sleeping lock
 * (a mutex held across the sleep), never under a spinlock (review finding 2) */
TEST(kernel_lock_resume_under_mutex) {
    LockdepThread t = {.level = {LOCK_LEVEL_INODE}, .depth = 1};
    ASSERT_TRUE(lockdep_check_kernel_lock(NULL, &t, 1) == NULL);
    ASSERT_TRUE(lockdep_check_kernel_lock(NULL, &t, 0) != NULL);
    static const Spinlock wq = SPINLOCK_INIT(LOCK_LEVEL_WAITQUEUE, "wq");
    const Spinlock *h[] = {&wq};
    LockdepCpu c = held_cpu(h, 1);
    ASSERT_TRUE(lockdep_check_kernel_lock(&c, NULL, 1) != NULL);
    ASSERT_TRUE(lockdep_check_kernel_lock(NULL, NULL, 0) == NULL);
    return 0;
}

/* --- Test suite export --- */

TestCase spinlock_tests[] = {
    TEST_ENTRY(init_state),
    TEST_ENTRY(lock_unlock_tracks_depth),
    TEST_ENTRY(trylock),
    TEST_ENTRY(irqsave_restores_flag),
    TEST_ENTRY(release_out_of_order_keeps_irqs_off),
    TEST_ENTRY(struct_size),
    TEST_ENTRY(contention),
    TEST_ENTRY(order_nothing_held),
    TEST_ENTRY(order_increasing_ok),
    TEST_ENTRY(order_inverted_pair_caught),
    TEST_ENTRY(order_checks_every_held_lock),
    TEST_ENTRY(order_same_level_needs_subkeys),
    TEST_ENTRY(order_run_queues_by_cpu),
    TEST_ENTRY(order_sleeping_locks),
    TEST_ENTRY(order_kernel_lock_first),
    TEST_ENTRY(kernel_lock_resume_under_mutex),
};

int spinlock_test_count = sizeof(spinlock_tests) / sizeof(spinlock_tests[0]);
