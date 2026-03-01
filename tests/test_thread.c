/* arc_os — Host-side tests for kernel/proc/thread.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_PROC_THREAD_H    /* Guard thread.h — we reproduce types below */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_MEM_H        /* Use libc memset/memcpy */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Reproduce types from thread.h (since we guard it) */
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

/* Allocation flags */
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01

/* Tracking kmalloc/kfree stubs */
static int kmalloc_call_count;
static int kfree_call_count;
static int kmalloc_force_fail;  /* If > 0, nth call returns NULL (1-indexed) */
static int kmalloc_call_seq;    /* Tracks call sequence for failure injection */

static void *kmalloc(size_t size, uint32_t flags) {
    kmalloc_call_count++;
    kmalloc_call_seq++;
    if (kmalloc_force_fail > 0 && kmalloc_call_seq == kmalloc_force_fail) {
        return NULL;
    }
    void *p = malloc(size);
    if (p && (flags & GFP_ZERO)) {
        memset(p, 0, size);
    }
    return p;
}

static void kfree(void *ptr) {
    kfree_call_count++;
    free(ptr);
}

/* Stub context_switch — never actually called from thread.c, but declared extern */
void context_switch(ThreadContext *old, ThreadContext *new_ctx) {
    (void)old; (void)new_ctx;
}

/* Forward-declare thread.c public functions (since we guarded thread.h) */
void thread_init(void);
Thread *thread_create(thread_entry_t entry, void *arg);
void thread_destroy(Thread *t);
Thread *thread_current(void);
void thread_set_current(Thread *t);

/* Include the real thread.c implementation */
#include "../kernel/proc/thread.c"

/* Reset test state — access statics directly since we included the .c */
static void reset_thread_state(void) {
    /* Free existing boot thread if any */
    if (current_thread != NULL) {
        free(current_thread);
        current_thread = NULL;
    }
    next_tid = 0;
    kmalloc_call_count = 0;
    kfree_call_count = 0;
    kmalloc_force_fail = 0;
    kmalloc_call_seq = 0;
}

/* --- Tests --- */

static int test_thread_init_creates_boot_thread(void) {
    reset_thread_state();
    thread_init();

    Thread *boot = thread_current();
    ASSERT_TRUE(boot != NULL);
    ASSERT_EQ(boot->tid, 0);
    ASSERT_EQ(boot->state, THREAD_RUNNING);
    ASSERT_TRUE(boot->stack_base == NULL);
    ASSERT_TRUE(boot->entry == NULL);
    return 0;
}

static int test_thread_init_sets_current(void) {
    reset_thread_state();
    thread_init();

    ASSERT_TRUE(thread_current() != NULL);
    ASSERT_EQ(thread_current()->tid, 0);
    return 0;
}

static int test_thread_create_basic(void) {
    reset_thread_state();
    thread_init();

    Thread *t = thread_create((thread_entry_t)0xDEAD, (void *)0xBEEF);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(t->tid, 1);
    ASSERT_EQ(t->state, THREAD_READY);
    ASSERT_EQ((uintptr_t)t->entry, 0xDEAD);
    ASSERT_EQ((uintptr_t)t->arg, 0xBEEF);

    thread_destroy(t);
    return 0;
}

static int test_thread_create_allocates_stack(void) {
    reset_thread_state();
    thread_init();

    Thread *t = thread_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(t != NULL);
    ASSERT_TRUE(t->stack_base != NULL);
    ASSERT_EQ(t->stack_size, THREAD_STACK_SIZE);

    thread_destroy(t);
    return 0;
}

static int test_thread_create_stack_setup(void) {
    reset_thread_state();
    thread_init();

    Thread *t = thread_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(t != NULL);

    /* RSP should be within the stack region */
    uint64_t stack_lo = (uint64_t)(uintptr_t)t->stack_base;
    uint64_t stack_hi = stack_lo + t->stack_size;
    ASSERT_TRUE(t->context.rsp >= stack_lo);
    ASSERT_TRUE(t->context.rsp < stack_hi);

    /* Value at RSP should be thread_trampoline address (return address for context_switch) */
    uint64_t *rsp_ptr = (uint64_t *)(uintptr_t)t->context.rsp;
    ASSERT_EQ(*rsp_ptr, (uint64_t)(uintptr_t)thread_trampoline);

    thread_destroy(t);
    return 0;
}

static int test_thread_create_callee_regs_zeroed(void) {
    reset_thread_state();
    thread_init();

    Thread *t = thread_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(t->context.r15, 0);
    ASSERT_EQ(t->context.r14, 0);
    ASSERT_EQ(t->context.r13, 0);
    ASSERT_EQ(t->context.r12, 0);
    ASSERT_EQ(t->context.rbx, 0);
    ASSERT_EQ(t->context.rbp, 0);

    thread_destroy(t);
    return 0;
}

static int test_thread_create_tid_sequence(void) {
    reset_thread_state();
    thread_init();  /* boot = tid 0 */

    Thread *t1 = thread_create((thread_entry_t)0xDEAD, NULL);
    Thread *t2 = thread_create((thread_entry_t)0xDEAD, NULL);
    Thread *t3 = thread_create((thread_entry_t)0xDEAD, NULL);

    ASSERT_EQ(t1->tid, 1);
    ASSERT_EQ(t2->tid, 2);
    ASSERT_EQ(t3->tid, 3);

    thread_destroy(t1);
    thread_destroy(t2);
    thread_destroy(t3);
    return 0;
}

static int test_thread_create_kmalloc_failure(void) {
    reset_thread_state();
    thread_init();  /* call 1: boot TCB */

    /* Fail the first kmalloc in thread_create (TCB allocation) */
    kmalloc_force_fail = 2;  /* 2nd overall call */
    Thread *t = thread_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(t == NULL);
    return 0;
}

static int test_thread_create_stack_kmalloc_failure(void) {
    reset_thread_state();
    thread_init();  /* call 1: boot TCB */

    /* Fail the second kmalloc in thread_create (stack allocation) */
    kmalloc_force_fail = 3;  /* 3rd overall call (init=1, create TCB=2, create stack=3) */
    Thread *t = thread_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(t == NULL);
    /* TCB should have been freed on stack alloc failure */
    ASSERT_TRUE(kfree_call_count >= 1);
    return 0;
}

static int test_thread_destroy_frees_resources(void) {
    reset_thread_state();
    thread_init();

    Thread *t = thread_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(t != NULL);

    kfree_call_count = 0;
    thread_destroy(t);
    /* Should free stack + TCB = 2 calls */
    ASSERT_EQ(kfree_call_count, 2);
    return 0;
}

static int test_thread_destroy_null_safe(void) {
    reset_thread_state();
    thread_init();

    /* Should not crash */
    thread_destroy(NULL);
    return 0;
}

static int test_thread_set_current_get_current(void) {
    reset_thread_state();
    thread_init();

    Thread fake;
    memset(&fake, 0, sizeof(fake));
    fake.tid = 42;

    thread_set_current(&fake);
    ASSERT_TRUE(thread_current() == &fake);
    ASSERT_EQ(thread_current()->tid, 42);
    return 0;
}

/* --- Test suite export --- */

TestCase thread_tests[] = {
    { "init_creates_boot_thread",    test_thread_init_creates_boot_thread },
    { "init_sets_current",           test_thread_init_sets_current },
    { "create_basic",                test_thread_create_basic },
    { "create_allocates_stack",      test_thread_create_allocates_stack },
    { "create_stack_setup",          test_thread_create_stack_setup },
    { "create_callee_regs_zeroed",   test_thread_create_callee_regs_zeroed },
    { "create_tid_sequence",         test_thread_create_tid_sequence },
    { "create_kmalloc_failure",      test_thread_create_kmalloc_failure },
    { "create_stack_alloc_failure",  test_thread_create_stack_kmalloc_failure },
    { "destroy_frees_resources",     test_thread_destroy_frees_resources },
    { "destroy_null_safe",           test_thread_destroy_null_safe },
    { "set_current_get_current",     test_thread_set_current_get_current },
};

int thread_test_count = sizeof(thread_tests) / sizeof(thread_tests[0]);
