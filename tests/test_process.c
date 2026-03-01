/* arc_os — Host-side tests for kernel/proc/process.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H        /* Use libc memset/memcpy */
#define ARCHOS_PROC_PROCESS_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Reproduce types (headers are guarded out) */
typedef uint32_t tid_t;
typedef uint32_t pid_t;

#define THREAD_CREATED  0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_DEAD     4

#define THREAD_STACK_SIZE  (16 * 1024)

typedef void (*thread_entry_t)(void *arg);

typedef struct {
    uint64_t r15, r14, r13, r12, rbx, rbp, rsp;
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

#define PROC_ALIVE       0
#define PROC_ZOMBIE      1
#define PROC_TERMINATED  2

typedef struct Process {
    pid_t           pid;
    uint8_t         state;
    Thread         *main_thread;
    uint64_t        page_table;
    struct Process *parent;
    struct Process *next;
} Process;

/* Allocation flags */
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01

/* Tracking kmalloc/kfree with failure injection */
static int kmalloc_call_count;
static int kfree_call_count;
static int kmalloc_force_fail;
static int kmalloc_call_seq;

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

/* thread_current / thread_set_current stubs (static to avoid linker clash) */
static Thread *test_current_thread = NULL;

static Thread *thread_current(void) {
    return test_current_thread;
}

static void thread_set_current(Thread *t) {
    test_current_thread = t;
}

/* Tracking thread_create stub (static to avoid linker clash) */
#define THREAD_POOL_SIZE 8
static Thread thread_pool[THREAD_POOL_SIZE];
static int thread_pool_next;
static int thread_create_call_count;
static int thread_create_force_fail;
static tid_t thread_next_tid = 1;  /* Boot thread is tid 0, created threads start at 1 */

static Thread *thread_create(thread_entry_t entry, void *arg) {
    thread_create_call_count++;
    if (thread_create_force_fail) {
        return NULL;
    }
    if (thread_pool_next >= THREAD_POOL_SIZE) return NULL;

    Thread *t = &thread_pool[thread_pool_next++];
    memset(t, 0, sizeof(Thread));
    t->tid = thread_next_tid++;
    t->state = THREAD_READY;
    t->entry = entry;
    t->arg = arg;
    return t;
}

/* Tracking sched_add_thread stub (static to avoid linker clash) */
static int sched_add_call_count;
static Thread *sched_add_last_thread;

static void sched_add_thread(Thread *t) {
    sched_add_call_count++;
    sched_add_last_thread = t;
    t->state = THREAD_READY;
}

/* Forward-declare process.c public functions (since we guarded process.h) */
void proc_init(void);
Process *proc_create(thread_entry_t entry, void *arg);
Process *proc_current(void);

/* Include the real process.c */
#include "../kernel/proc/process.c"

/* Boot thread for tests */
static Thread boot_thread;

static void setup_boot_thread(void) {
    memset(&boot_thread, 0, sizeof(Thread));
    boot_thread.tid = 0;
    boot_thread.state = THREAD_RUNNING;
    test_current_thread = &boot_thread;
}

static void reset_proc_state(void) {
    proc_list = NULL;
    next_pid = 0;
    memset(proc_table, 0, sizeof(proc_table));

    kmalloc_call_count = 0;
    kfree_call_count = 0;
    kmalloc_force_fail = 0;
    kmalloc_call_seq = 0;

    thread_create_call_count = 0;
    thread_create_force_fail = 0;
    thread_pool_next = 0;
    thread_next_tid = 1;
    memset(thread_pool, 0, sizeof(thread_pool));

    sched_add_call_count = 0;
    sched_add_last_thread = NULL;

    setup_boot_thread();
}

/* --- Tests --- */

static int test_proc_init_creates_boot_process(void) {
    reset_proc_state();
    proc_init();

    Process *p = proc_current();
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(p->pid, 0);
    ASSERT_EQ(p->state, PROC_ALIVE);
    ASSERT_TRUE(p->main_thread == &boot_thread);
    ASSERT_TRUE(proc_table[0] == p);
    return 0;
}

static int test_proc_init_pid_sequence(void) {
    reset_proc_state();
    proc_init();

    ASSERT_EQ(next_pid, 1);
    return 0;
}

static int test_proc_create_basic(void) {
    reset_proc_state();
    proc_init();

    Process *p = proc_create((thread_entry_t)0xDEAD, (void *)0xBEEF);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(p->pid, 1);
    ASSERT_EQ(p->state, PROC_ALIVE);
    ASSERT_EQ(thread_create_call_count, 1);
    ASSERT_EQ(sched_add_call_count, 1);
    return 0;
}

static int test_proc_create_links_in_list(void) {
    reset_proc_state();
    proc_init();

    Process *p1 = proc_create((thread_entry_t)0xDEAD, NULL);
    Process *p2 = proc_create((thread_entry_t)0xBEEF, NULL);

    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(p2 != NULL);

    /* proc_list is prepend — p2 at head, then p1, then boot */
    ASSERT_TRUE(proc_list == p2);
    ASSERT_TRUE(p2->next == p1);
    /* p1->next is the boot process */
    ASSERT_TRUE(p1->next != NULL);
    ASSERT_EQ(p1->next->pid, 0);
    return 0;
}

static int test_proc_create_thread_failure(void) {
    reset_proc_state();
    proc_init();

    thread_create_force_fail = 1;
    Process *p = proc_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(p == NULL);
    /* kmalloc for Process should have been freed */
    ASSERT_TRUE(kfree_call_count >= 1);
    return 0;
}

static int test_proc_create_kmalloc_failure(void) {
    reset_proc_state();
    proc_init();  /* uses call 1 */

    /* Fail the next kmalloc (Process allocation in proc_create) */
    kmalloc_force_fail = 2;
    Process *p = proc_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(p == NULL);
    return 0;
}

static int test_proc_current_returns_boot(void) {
    reset_proc_state();
    proc_init();

    Process *p = proc_current();
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(p->pid, 0);
    return 0;
}

static int test_proc_create_pid_sequence(void) {
    reset_proc_state();
    proc_init();

    Process *p1 = proc_create((thread_entry_t)0xDEAD, NULL);
    Process *p2 = proc_create((thread_entry_t)0xDEAD, NULL);
    Process *p3 = proc_create((thread_entry_t)0xDEAD, NULL);

    ASSERT_EQ(p1->pid, 1);
    ASSERT_EQ(p2->pid, 2);
    ASSERT_EQ(p3->pid, 3);
    return 0;
}

static int test_proc_table_stores_entries(void) {
    reset_proc_state();
    proc_init();

    Process *p1 = proc_create((thread_entry_t)0xDEAD, NULL);
    Process *p2 = proc_create((thread_entry_t)0xDEAD, NULL);

    /* Boot process at tid=0, p1's thread at tid=1, p2's thread at tid=2 */
    ASSERT_TRUE(proc_table[0] != NULL);
    ASSERT_EQ(proc_table[0]->pid, 0);
    ASSERT_TRUE(proc_table[1] == p1);
    ASSERT_TRUE(proc_table[2] == p2);
    return 0;
}

/* --- Test suite export --- */

TestCase process_tests[] = {
    { "init_creates_boot_process",  test_proc_init_creates_boot_process },
    { "init_pid_sequence",          test_proc_init_pid_sequence },
    { "create_basic",               test_proc_create_basic },
    { "create_links_in_list",       test_proc_create_links_in_list },
    { "create_thread_failure",      test_proc_create_thread_failure },
    { "create_kmalloc_failure",     test_proc_create_kmalloc_failure },
    { "current_returns_boot",       test_proc_current_returns_boot },
    { "create_pid_sequence",        test_proc_create_pid_sequence },
    { "table_stores_entries",       test_proc_table_stores_entries },
};

int process_test_count = sizeof(process_tests) / sizeof(process_tests[0]);
