/* arc_os — Host-side tests for kernel/proc/waitqueue.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_PROC_SPINLOCK_H
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_PROC_WAITQUEUE_H

/* Reproduce Thread/ThreadContext types (guarded out thread.h) */
typedef uint32_t tid_t;

#define THREAD_CREATED  0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_DEAD     4

#define THREAD_STACK_SIZE  (16 * 1024)

typedef void (*thread_entry_t)(void *arg);

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsp;
} ThreadContext;

typedef struct Thread {
    tid_t           tid;
    uint8_t         state;
    ThreadContext   context;
    uint8_t        *stack_base;
    size_t          stack_size;
    uint64_t        kernel_stack_top;
    thread_entry_t  entry;
    void           *arg;
    struct Thread  *next;
} Thread;

/* Spinlock stub — no-op for host tests */
typedef struct {
    volatile uint32_t locked;
    uint64_t saved_flags;
} Spinlock;

#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }

static inline void spinlock_acquire(Spinlock *lock) {
    lock->locked = 1;
}

static inline void spinlock_release(Spinlock *lock) {
    lock->locked = 0;
}

/* WaitQueue type (guarded out waitqueue.h) */
typedef struct WaitQueue {
    Spinlock lock;
    Thread  *head;
    Thread  *tail;
} WaitQueue;

#define WAITQUEUE_INIT { .lock = SPINLOCK_INIT, .head = NULL, .tail = NULL }

/* thread_current stub */
static Thread *test_current_thread = NULL;

static Thread *thread_current(void) {
    return test_current_thread;
}

/* sched_yield stub — no-op (lets us observe side effects without context switch) */
static int yield_count = 0;

static void sched_yield(void) {
    yield_count++;
}

/* sched_add_thread tracking stub */
#define MAX_WOKEN 16
static Thread *woken_threads[MAX_WOKEN];
static int woken_count = 0;

static void sched_add_thread(Thread *t) {
    t->state = THREAD_READY;
    if (woken_count < MAX_WOKEN) {
        woken_threads[woken_count++] = t;
    }
}

/* Include the real waitqueue.c */
#include "../kernel/proc/waitqueue.c"

/* Thread pool for tests */
#define POOL_SIZE 8
static Thread thread_pool[POOL_SIZE];

static Thread *make_thread(int index, tid_t tid, uint8_t state) {
    memset(&thread_pool[index], 0, sizeof(Thread));
    thread_pool[index].tid = tid;
    thread_pool[index].state = state;
    thread_pool[index].next = NULL;
    return &thread_pool[index];
}

static void reset_state(void) {
    test_current_thread = NULL;
    yield_count = 0;
    woken_count = 0;
    memset(woken_threads, 0, sizeof(woken_threads));
    memset(thread_pool, 0, sizeof(thread_pool));
}

/* --- Tests --- */

static int test_init_sets_null(void) {
    reset_state();
    WaitQueue wq;
    wq.head = (Thread *)0xDEAD;
    wq.tail = (Thread *)0xBEEF;

    wq_init(&wq);

    ASSERT_TRUE(wq.head == NULL);
    ASSERT_TRUE(wq.tail == NULL);
    ASSERT_EQ(wq.lock.locked, 0);
    return 0;
}

static int test_wake_empty_returns_zero(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;

    int ret = wq_wake(&wq);

    ASSERT_EQ(ret, 0);
    ASSERT_EQ(woken_count, 0);
    return 0;
}

static int test_wake_all_empty_returns_zero(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;

    int ret = wq_wake_all(&wq);

    ASSERT_EQ(ret, 0);
    ASSERT_EQ(woken_count, 0);
    return 0;
}

static int test_sleep_sets_blocked(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;
    cond_lock.locked = 1; /* Caller holds it */

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    test_current_thread = a;

    wq_sleep(&wq, &cond_lock);

    ASSERT_EQ(a->state, THREAD_BLOCKED);
    return 0;
}

static int test_sleep_releases_caller_lock(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;
    cond_lock.locked = 1;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    test_current_thread = a;

    wq_sleep(&wq, &cond_lock);

    ASSERT_EQ(cond_lock.locked, 0);
    return 0;
}

static int test_sleep_adds_to_queue(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;
    cond_lock.locked = 1;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    test_current_thread = a;

    wq_sleep(&wq, &cond_lock);

    ASSERT_TRUE(wq.head == a);
    ASSERT_TRUE(wq.tail == a);
    return 0;
}

static int test_wake_removes_from_queue(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;
    cond_lock.locked = 1;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    test_current_thread = a;

    wq_sleep(&wq, &cond_lock);
    wq_wake(&wq);

    ASSERT_TRUE(wq.head == NULL);
    ASSERT_TRUE(wq.tail == NULL);
    return 0;
}

static int test_wake_calls_sched_add(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;
    cond_lock.locked = 1;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    test_current_thread = a;

    wq_sleep(&wq, &cond_lock);
    wq_wake(&wq);

    ASSERT_EQ(woken_count, 1);
    ASSERT_TRUE(woken_threads[0] == a);
    ASSERT_EQ(a->state, THREAD_READY);
    return 0;
}

static int test_wake_returns_one(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;
    cond_lock.locked = 1;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    test_current_thread = a;

    wq_sleep(&wq, &cond_lock);
    int ret = wq_wake(&wq);

    ASSERT_EQ(ret, 1);
    return 0;
}

static int test_fifo_ordering(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_RUNNING);

    /* Sleep thread A */
    cond_lock.locked = 1;
    test_current_thread = a;
    wq_sleep(&wq, &cond_lock);

    /* Sleep thread B */
    cond_lock.locked = 1;
    test_current_thread = b;
    wq_sleep(&wq, &cond_lock);

    /* Wake first — should be A (FIFO) */
    wq_wake(&wq);
    ASSERT_EQ(woken_count, 1);
    ASSERT_TRUE(woken_threads[0] == a);

    /* Wake second — should be B */
    wq_wake(&wq);
    ASSERT_EQ(woken_count, 2);
    ASSERT_TRUE(woken_threads[1] == b);

    return 0;
}

static int test_wake_all_wakes_multiple(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_RUNNING);
    Thread *c = make_thread(2, 3, THREAD_RUNNING);

    /* Sleep all three */
    cond_lock.locked = 1;
    test_current_thread = a;
    wq_sleep(&wq, &cond_lock);

    cond_lock.locked = 1;
    test_current_thread = b;
    wq_sleep(&wq, &cond_lock);

    cond_lock.locked = 1;
    test_current_thread = c;
    wq_sleep(&wq, &cond_lock);

    /* Wake all — should wake A, B, C in FIFO order */
    wq_wake_all(&wq);

    ASSERT_EQ(woken_count, 3);
    ASSERT_TRUE(woken_threads[0] == a);
    ASSERT_TRUE(woken_threads[1] == b);
    ASSERT_TRUE(woken_threads[2] == c);

    return 0;
}

static int test_wake_all_returns_count(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_RUNNING);
    Thread *c = make_thread(2, 3, THREAD_RUNNING);

    cond_lock.locked = 1;
    test_current_thread = a;
    wq_sleep(&wq, &cond_lock);

    cond_lock.locked = 1;
    test_current_thread = b;
    wq_sleep(&wq, &cond_lock);

    cond_lock.locked = 1;
    test_current_thread = c;
    wq_sleep(&wq, &cond_lock);

    int ret = wq_wake_all(&wq);

    ASSERT_EQ(ret, 3);
    return 0;
}

static int test_sleep_wake_reuse(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);

    /* First cycle: sleep + wake */
    cond_lock.locked = 1;
    test_current_thread = a;
    wq_sleep(&wq, &cond_lock);
    wq_wake(&wq);

    ASSERT_EQ(woken_count, 1);
    ASSERT_TRUE(wq.head == NULL);

    /* Second cycle: sleep + wake again */
    a->state = THREAD_RUNNING;
    cond_lock.locked = 1;
    test_current_thread = a;
    wq_sleep(&wq, &cond_lock);
    wq_wake(&wq);

    ASSERT_EQ(woken_count, 2);
    ASSERT_TRUE(woken_threads[1] == a);
    ASSERT_TRUE(wq.head == NULL);

    return 0;
}

static int test_multiple_wakes_empty(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;

    /* Repeated wakes on empty queue — all return 0, no crash */
    ASSERT_EQ(wq_wake(&wq), 0);
    ASSERT_EQ(wq_wake(&wq), 0);
    ASSERT_EQ(wq_wake(&wq), 0);
    ASSERT_EQ(woken_count, 0);
    return 0;
}

static int test_wake_all_empties_queue(void) {
    reset_state();
    WaitQueue wq = (WaitQueue)WAITQUEUE_INIT;
    Spinlock cond_lock = (Spinlock)SPINLOCK_INIT;

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_RUNNING);

    cond_lock.locked = 1;
    test_current_thread = a;
    wq_sleep(&wq, &cond_lock);

    cond_lock.locked = 1;
    test_current_thread = b;
    wq_sleep(&wq, &cond_lock);

    wq_wake_all(&wq);

    ASSERT_TRUE(wq.head == NULL);
    ASSERT_TRUE(wq.tail == NULL);
    return 0;
}

/* --- Test suite export --- */

TestCase waitqueue_tests[] = {
    { "init_sets_null",           test_init_sets_null },
    { "wake_empty_returns_zero",  test_wake_empty_returns_zero },
    { "wake_all_empty_returns_zero", test_wake_all_empty_returns_zero },
    { "sleep_sets_blocked",       test_sleep_sets_blocked },
    { "sleep_releases_caller_lock", test_sleep_releases_caller_lock },
    { "sleep_adds_to_queue",      test_sleep_adds_to_queue },
    { "wake_removes_from_queue",  test_wake_removes_from_queue },
    { "wake_calls_sched_add",     test_wake_calls_sched_add },
    { "wake_returns_one",         test_wake_returns_one },
    { "fifo_ordering",            test_fifo_ordering },
    { "wake_all_wakes_multiple",  test_wake_all_wakes_multiple },
    { "wake_all_returns_count",   test_wake_all_returns_count },
    { "sleep_wake_reuse",         test_sleep_wake_reuse },
    { "multiple_wakes_empty",     test_multiple_wakes_empty },
    { "wake_all_empties_queue",   test_wake_all_empties_queue },
};

int waitqueue_test_count = sizeof(waitqueue_tests) / sizeof(waitqueue_tests[0]);
