/* arc_os — Host-side tests for kernel/proc/sched.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_PROC_SPINLOCK_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_SCHED_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

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

/* thread_current/thread_set_current stubs (static to avoid linker clash) */
static Thread *test_current_thread = NULL;

static Thread *thread_current(void) {
    return test_current_thread;
}

static void thread_set_current(Thread *t) {
    test_current_thread = t;
}

/* Tracking context_switch stub (static to avoid linker clash) */
static int ctx_switch_count;
static ThreadContext *ctx_switch_old;
static ThreadContext *ctx_switch_new;

static void context_switch(ThreadContext *old, ThreadContext *new_ctx) {
    ctx_switch_count++;
    ctx_switch_old = old;
    ctx_switch_new = new_ctx;
}

/* Include the real sched.c */
#include "../kernel/proc/sched.c"

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

static void reset_sched_state(void) {
    queue_head = NULL;
    queue_tail = NULL;
    idle_thread = NULL;
    sched_lock = (Spinlock)SPINLOCK_INIT;
    test_current_thread = NULL;
    ctx_switch_count = 0;
    ctx_switch_old = NULL;
    ctx_switch_new = NULL;
    memset(thread_pool, 0, sizeof(thread_pool));
}

/* --- Tests --- */

static int test_sched_init(void) {
    reset_sched_state();
    /* Put garbage in the queue pointers */
    queue_head = (Thread *)0xDEAD;
    queue_tail = (Thread *)0xBEEF;

    sched_init();

    ASSERT_TRUE(queue_head == NULL);
    ASSERT_TRUE(queue_tail == NULL);
    ASSERT_TRUE(idle_thread == NULL);
    return 0;
}

static int test_sched_add_one_thread(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_CREATED);
    sched_add_thread(a);

    ASSERT_TRUE(queue_head == a);
    ASSERT_TRUE(queue_tail == a);
    ASSERT_EQ(a->state, THREAD_READY);
    return 0;
}

static int test_sched_add_multiple_fifo(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_CREATED);
    Thread *b = make_thread(1, 2, THREAD_CREATED);
    Thread *c = make_thread(2, 3, THREAD_CREATED);

    sched_add_thread(a);
    sched_add_thread(b);
    sched_add_thread(c);

    /* FIFO: head=A, A->next=B, B->next=C, tail=C */
    ASSERT_TRUE(queue_head == a);
    ASSERT_TRUE(a->next == b);
    ASSERT_TRUE(b->next == c);
    ASSERT_TRUE(queue_tail == c);
    return 0;
}

static int test_sched_remove_head(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_READY);
    Thread *b = make_thread(1, 2, THREAD_READY);
    Thread *c = make_thread(2, 3, THREAD_READY);

    sched_add_thread(a);
    sched_add_thread(b);
    sched_add_thread(c);

    sched_remove_thread(a);

    ASSERT_TRUE(queue_head == b);
    ASSERT_TRUE(b->next == c);
    ASSERT_TRUE(queue_tail == c);
    ASSERT_TRUE(a->next == NULL);
    return 0;
}

static int test_sched_remove_middle(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_READY);
    Thread *b = make_thread(1, 2, THREAD_READY);
    Thread *c = make_thread(2, 3, THREAD_READY);

    sched_add_thread(a);
    sched_add_thread(b);
    sched_add_thread(c);

    sched_remove_thread(b);

    ASSERT_TRUE(queue_head == a);
    ASSERT_TRUE(a->next == c);
    ASSERT_TRUE(queue_tail == c);
    return 0;
}

static int test_sched_remove_tail(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_READY);
    Thread *b = make_thread(1, 2, THREAD_READY);
    Thread *c = make_thread(2, 3, THREAD_READY);

    sched_add_thread(a);
    sched_add_thread(b);
    sched_add_thread(c);

    sched_remove_thread(c);

    ASSERT_TRUE(queue_head == a);
    ASSERT_TRUE(a->next == b);
    ASSERT_TRUE(queue_tail == b);
    ASSERT_TRUE(b->next == NULL);
    return 0;
}

static int test_sched_remove_not_present(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_READY);
    Thread *b = make_thread(1, 2, THREAD_READY);
    Thread *c = make_thread(2, 3, THREAD_READY);  /* Not added */

    sched_add_thread(a);
    sched_add_thread(b);

    sched_remove_thread(c);  /* Not in queue — should be no-op */

    ASSERT_TRUE(queue_head == a);
    ASSERT_TRUE(a->next == b);
    ASSERT_TRUE(queue_tail == b);
    return 0;
}

static int test_sched_schedule_basic(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_READY);

    test_current_thread = a;
    sched_add_thread(b);

    sched_schedule();

    /* B should now be current and RUNNING */
    ASSERT_TRUE(test_current_thread == b);
    ASSERT_EQ(b->state, THREAD_RUNNING);

    /* A should have been re-enqueued as READY */
    ASSERT_EQ(a->state, THREAD_READY);
    ASSERT_TRUE(queue_head == a);

    /* context_switch should have been called */
    ASSERT_EQ(ctx_switch_count, 1);
    ASSERT_TRUE(ctx_switch_old == &a->context);
    ASSERT_TRUE(ctx_switch_new == &b->context);
    return 0;
}

static int test_sched_schedule_empty_keeps_current(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    test_current_thread = a;

    /* Empty queue — current stays */
    sched_schedule();

    ASSERT_TRUE(test_current_thread == a);
    ASSERT_EQ(a->state, THREAD_RUNNING);
    ASSERT_EQ(ctx_switch_count, 0);
    return 0;
}

static int test_sched_schedule_idle_fallback(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_BLOCKED);  /* Can't continue */
    Thread *idle = make_thread(1, 0, THREAD_RUNNING);

    test_current_thread = a;
    sched_set_idle_thread(idle);

    sched_schedule();

    /* idle should become current */
    ASSERT_TRUE(test_current_thread == idle);
    ASSERT_EQ(idle->state, THREAD_RUNNING);
    /* A is BLOCKED, should NOT be re-enqueued */
    ASSERT_TRUE(queue_head == NULL);
    /* context_switch called */
    ASSERT_EQ(ctx_switch_count, 1);
    return 0;
}

static int test_sched_idle_not_requeued(void) {
    reset_sched_state();
    sched_init();

    Thread *idle = make_thread(0, 0, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_READY);

    sched_set_idle_thread(idle);
    test_current_thread = idle;
    sched_add_thread(b);

    sched_schedule();

    /* B should be current */
    ASSERT_TRUE(test_current_thread == b);
    ASSERT_EQ(b->state, THREAD_RUNNING);

    /* idle should NOT be in the run queue */
    ASSERT_TRUE(queue_head == NULL);
    ASSERT_TRUE(queue_tail == NULL);

    ASSERT_EQ(ctx_switch_count, 1);
    return 0;
}

static int test_sched_yield_calls_schedule(void) {
    reset_sched_state();
    sched_init();

    Thread *a = make_thread(0, 1, THREAD_RUNNING);
    Thread *b = make_thread(1, 2, THREAD_READY);

    test_current_thread = a;
    sched_add_thread(b);

    sched_yield();

    /* Lock should have been acquired and released */
    ASSERT_EQ(sched_lock.locked, 0);

    /* Schedule should have run — B is current */
    ASSERT_TRUE(test_current_thread == b);
    ASSERT_EQ(ctx_switch_count, 1);
    return 0;
}

/* --- Test suite export --- */

TestCase sched_tests[] = {
    { "init",                     test_sched_init },
    { "add_one_thread",           test_sched_add_one_thread },
    { "add_multiple_fifo",        test_sched_add_multiple_fifo },
    { "remove_head",              test_sched_remove_head },
    { "remove_middle",            test_sched_remove_middle },
    { "remove_tail",              test_sched_remove_tail },
    { "remove_not_present",       test_sched_remove_not_present },
    { "schedule_basic",           test_sched_schedule_basic },
    { "schedule_empty_keeps_cur", test_sched_schedule_empty_keeps_current },
    { "schedule_idle_fallback",   test_sched_schedule_idle_fallback },
    { "idle_not_requeued",        test_sched_idle_not_requeued },
    { "yield_calls_schedule",     test_sched_yield_calls_schedule },
};

int sched_test_count = sizeof(sched_tests) / sizeof(sched_tests[0]);
