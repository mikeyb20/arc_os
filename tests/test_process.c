/* arc_os — Host-side tests for kernel/proc/process.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_MM_VMM_H          /* Guard vmm.h — we stub its functions */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H        /* Use libc memset/memcpy */
#define ARCHOS_PROC_PROCESS_H
#define ARCHOS_PROC_SIGNAL_H
#define ARCHOS_PROC_WAITQUEUE_H
#define ARCHOS_PROC_FD_H
#define ARCHOS_PROC_PROC_SYSCALLS_H  /* proc_release_fds: stubbed below */
#define ARCHOS_ARCH_X86_64_USERMODE_H
#define ARCHOS_ARCH_X86_64_GDT_H
#define ARCHOS_ARCH_X86_64_SYSCALL_H
#define ARCHOS_ARCH_X86_64_PAGING_H
#define ARCHOS_FS_PATH_H
#define ARCHOS_ARCH_HAL_H        /* hal.h: only fpu.h is used, stubbed below */
#define ARCHOS_ARCH_X86_64_FPU_H  /* fpu.h: stubbed below */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }
#define KERNEL_PANIC() do { } while(0)

#define EINVAL 22
#define PATH_MAX 512

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
    uint64_t        kernel_stack_top;
    thread_entry_t  entry;
    void           *arg;
    void           *fpu_state;
    uint64_t        fs_base;
    uint64_t        clear_child_tid;
    struct Process *proc;
    struct Thread  *next;
} Thread;

#define PROC_ALIVE       0
#define PROC_ZOMBIE      1
#define PROC_TERMINATED  2
#define PROC_STOPPED     3

typedef struct FdTable FdTable;

/* Signal types stub */
typedef void (*sig_handler_t)(int);
#define SIG_DFL  ((sig_handler_t)0)
#define NSIG 32

typedef struct {
    uint8_t  trampoline[16];
    uint64_t user_rip, user_rsp, user_rflags;
    uint64_t rax, rbx, rbp, r12, r13, r14, r15;
    uint64_t rdi, rsi, rdx, r8, r9, r10;
    uint64_t signo, ret_addr;
} SignalFrame;

typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx, rcx, r11, rsp;
} SyscallFrame;

typedef struct {
    uint32_t      pending;
    sig_handler_t handlers[NSIG];
    uint8_t       restoring;
    SignalFrame   restore_frame;
} SigState;

static void sig_init(SigState *ss) {
    ss->pending = 0;
    ss->restoring = 0;
    for (int i = 0; i < NSIG; i++) ss->handlers[i] = SIG_DFL;
}

/* WaitQueue stub */
typedef struct WaitQueue { int dummy; } WaitQueue;
static void wq_init(WaitQueue *wq) { (void)wq; }

/* Saved user context for fork */
typedef struct ForkContext {
    uint64_t user_rip;
    uint64_t user_rsp;
    uint64_t user_rflags;
    uint64_t user_rbp;
    uint64_t user_rbx;
    uint64_t user_r12;
    uint64_t user_r13;
    uint64_t user_r14;
    uint64_t user_r15;
} ForkContext;

typedef struct Process {
    pid_t           pid;
    pid_t           pgid;
    uint8_t         state;
    int32_t         exit_status;
    Thread         *main_thread;
    uint64_t        page_table;
    FdTable        *fd_table;
    uint64_t        brk_current;
    uint64_t        brk_start;
    char            cwd[PATH_MAX];
    uint32_t        uid;
    uint32_t        gid;
    uint32_t        euid;
    uint32_t        egid;
    uint32_t        umask;
    SigState        sig;
    WaitQueue       child_exit_wq;
    struct Process *parent;
    struct Process *prev;
    struct Process *next;
} Process;

/* VMM stubs: count address spaces freed */
static int vmm_free_calls;
static uint64_t vmm_free_last;
static uint64_t vmm_create_user_pml4(void) {
    return 0x300000;
}
static uint64_t vmm_fork_address_space(uint64_t src) { (void)src; return 0x400000; }
static void vmm_free_user_pages(uint64_t pml4) {
    vmm_free_calls++;
    vmm_free_last = pml4;
}

/* FD stubs: fork's table copy is a real allocation, so reaping must free it */
struct FdTable {
    int dummy;
};
static void *kmalloc(size_t size, uint32_t flags);
static FdTable *fd_table_dup(const FdTable *src) {
    return src != NULL ? kmalloc(sizeof(FdTable), 0) : NULL;
}
static int release_fds_calls;
static void proc_release_fds(Process *p) {
    (void)p;
    release_fds_calls++;
}

/* Arch stubs */
static void gdt_set_kernel_stack(uint64_t rsp0) { (void)rsp0; }
static void paging_write_cr3(uint64_t cr3) { (void)cr3; }
static uint64_t syscall_kernel_rsp;
__attribute__((noreturn))
static void fork_return_to_user(const ForkContext *ctx) {
    (void)ctx;
    /* Should never be called in tests */
    for (;;) {}
}

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
    if (ptr == NULL) return;      /* like the kernel's kfree: NULL is a no-op */
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

/* thread_destroy stub: records which threads were freed */
static int thread_destroy_calls;
static Thread *thread_destroy_last;
static void thread_destroy(Thread *t) {
    thread_destroy_calls++;
    thread_destroy_last = t;
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
Process *proc_create_user(void);
Process *proc_current(void);
void proc_set_main_thread(Process *p, Thread *t);
Process *proc_get_by_pid(uint32_t pid);
Process *proc_fork(Process *parent, const ForkContext *user_ctx);
Process *proc_find_zombie_child(Process *parent);
int proc_reap(Process *child, int32_t *status_out);
int proc_has_children(Process *parent);
void proc_set_init(Process *p);
Process *proc_reparent_children(Process *p);

/* fpu.h stub: records the fork child's FPU area being filled */
static int fpu_save_calls;
static void *fpu_save_last;
static void fpu_save_current(void *dst) { fpu_save_calls++; fpu_save_last = dst; }

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
    init_proc = NULL;
    vmm_free_calls = 0;
    vmm_free_last = 0;
    release_fds_calls = 0;
    thread_destroy_calls = 0;
    thread_destroy_last = NULL;

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
    ASSERT_TRUE(boot_thread.proc == p);
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

static int test_threads_point_at_process(void) {
    reset_proc_state();
    proc_init();

    Process *p1 = proc_create((thread_entry_t)0xDEAD, NULL);
    Process *p2 = proc_create((thread_entry_t)0xDEAD, NULL);

    ASSERT_EQ(boot_thread.proc->pid, 0);
    ASSERT_TRUE(p1->main_thread->proc == p1);
    ASSERT_TRUE(p2->main_thread->proc == p2);
    return 0;
}

/* A thread with tid >= 64 still finds its process (the old tid-indexed table
 * returned NULL past 64 threads) */
static int test_current_with_large_tid(void) {
    reset_proc_state();
    proc_init();
    thread_next_tid = 1000;
    Process *p = proc_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(p->main_thread->tid, 1000);
    test_current_thread = p->main_thread;
    ASSERT_TRUE(proc_current() == p);
    return 0;
}

/* --- Additional process tests --- */

static int test_state_alive_to_zombie(void) {
    reset_proc_state();
    proc_init();
    Process *p = proc_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(p->state, PROC_ALIVE);
    p->state = PROC_ZOMBIE;
    ASSERT_EQ(p->state, PROC_ZOMBIE);
    return 0;
}

static int test_state_values_distinct(void) {
    ASSERT_NEQ(PROC_ALIVE, PROC_ZOMBIE);
    ASSERT_NEQ(PROC_ZOMBIE, PROC_TERMINATED);
    ASSERT_NEQ(PROC_ALIVE, PROC_TERMINATED);
    return 0;
}

static int test_max_processes_boundary(void) {
    reset_proc_state();
    proc_init();
    /* Create THREAD_POOL_SIZE - 1 processes (one slot used by boot) */
    for (int i = 0; i < THREAD_POOL_SIZE - 1; i++) {
        Process *p = proc_create((thread_entry_t)0xDEAD, NULL);
        ASSERT_TRUE(p != NULL);
    }
    /* Every thread maps back to its process */
    for (int i = 0; i < THREAD_POOL_SIZE - 1; i++) {
        ASSERT_TRUE(thread_pool[i].proc != NULL);
        ASSERT_TRUE(thread_pool[i].proc->main_thread == &thread_pool[i]);
    }
    return 0;
}

static int test_parent_null_by_default(void) {
    reset_proc_state();
    proc_init();
    Process *p = proc_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(p->parent == NULL);
    return 0;
}

static int test_current_after_thread_switch(void) {
    reset_proc_state();
    proc_init();
    Process *p1 = proc_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(p1 != NULL);

    /* Simulate thread switch by changing test_current_thread */
    test_current_thread = p1->main_thread;
    Process *cur = proc_current();
    ASSERT_TRUE(cur == p1);
    ASSERT_EQ(cur->pid, p1->pid);

    /* Switch back to boot thread */
    test_current_thread = &boot_thread;
    cur = proc_current();
    ASSERT_EQ(cur->pid, 0);
    return 0;
}

/* --- Reaping and re-parenting (M0-10) --- */

static int list_contains(const Process *p) {
    for (Process *q = proc_list; q != NULL; q = q->next) {
        if (q == p) return 1;
    }
    return 0;
}

/* The list stays consistent in both directions */
static int list_well_formed(void) {
    Process *prev = NULL;
    for (Process *q = proc_list; q != NULL; q = q->next) {
        if (q->prev != prev) return 0;
        prev = q;
    }
    return 1;
}

/* A forked child, as wait4 sees it after exit: a zombie with its own address
 * space, fd table and thread */
static Process *make_zombie_child(Process *parent) {
    ForkContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    Process *c = proc_fork(parent, &ctx);
    if (c == NULL) return NULL;
    c->state = PROC_ZOMBIE;
    c->exit_status = 7;
    return c;
}

static Process *make_parent(void) {
    Process *p = proc_create((thread_entry_t)0xDEAD, NULL);
    if (p == NULL) return NULL;
    p->page_table = 0x300000;
    static FdTable parent_fds;
    p->fd_table = &parent_fds;
    test_current_thread = p->main_thread;
    return p;
}

static int test_reap_frees_everything(void) {
    reset_proc_state();
    proc_init();
    Process *parent = make_parent();
    ASSERT_TRUE(parent != NULL);
    Process *c = make_zombie_child(parent);
    ASSERT_TRUE(c != NULL);
    Thread *ct = c->main_thread;
    uint64_t pml4 = c->page_table;
    pid_t pid = c->pid;
    int live_before = kmalloc_call_count - kfree_call_count;

    int32_t status = 0;
    ASSERT_EQ(proc_reap(c, &status), (int)pid);
    ASSERT_EQ(status, 7);
    ASSERT_EQ(vmm_free_calls, 1);                 /* user pages + page tables */
    ASSERT_EQ(vmm_free_last, pml4);
    ASSERT_EQ(thread_destroy_calls, 1);           /* stack, FPU area, TCB */
    ASSERT_TRUE(thread_destroy_last == ct);
    /* The fd table and the PCB: the two allocations fork made that remain */
    ASSERT_EQ(kmalloc_call_count - kfree_call_count, live_before - 2);
    ASSERT_TRUE(proc_get_by_pid(pid) == NULL);
    return 0;
}

static int test_reap_unlinks_head_middle_tail(void) {
    reset_proc_state();
    proc_init();
    Process *parent = make_parent();
    Process *a = make_zombie_child(parent);       /* list: c b a parent boot */
    Process *b = make_zombie_child(parent);
    Process *c = make_zombie_child(parent);
    ASSERT_TRUE(a && b && c);
    ASSERT_TRUE(proc_list == c && list_well_formed());

    ASSERT_TRUE(proc_reap(b, NULL) > 0);          /* middle */
    ASSERT_TRUE(list_well_formed() && !list_contains(b) && c->next == a);
    ASSERT_TRUE(proc_reap(c, NULL) > 0);          /* head */
    ASSERT_TRUE(proc_list == a && a->prev == NULL && list_well_formed());
    ASSERT_TRUE(proc_reap(a, NULL) > 0);
    ASSERT_TRUE(proc_list == parent && list_well_formed());
    ASSERT_EQ(proc_has_children(parent), 0);
    return 0;
}

static int test_reap_rejects_live_child(void) {
    reset_proc_state();
    proc_init();
    Process *parent = make_parent();
    ForkContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    Process *c = proc_fork(parent, &ctx);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(proc_reap(c, NULL), -EINVAL);
    ASSERT_EQ(vmm_free_calls, 0);
    ASSERT_TRUE(list_contains(c));
    return 0;
}

static int test_reparent_to_init(void) {
    reset_proc_state();
    proc_init();
    Process *init = make_parent();
    proc_set_init(init);
    Process *mid = make_zombie_child(init);
    mid->state = PROC_ALIVE;
    test_current_thread = mid->main_thread;
    mid->page_table = 0x300000;
    static FdTable mid_fds;
    mid->fd_table = &mid_fds;
    Process *live = make_zombie_child(mid);
    live->state = PROC_ALIVE;
    Process *other = make_zombie_child(init);     /* not mid's: untouched */

    /* No zombie among mid's children: init needs no wake-up */
    ASSERT_TRUE(proc_reparent_children(mid) == NULL);
    ASSERT_TRUE(live->parent == init);
    ASSERT_TRUE(other->parent == init);

    /* A zombie handed over: init is returned so the caller wakes it */
    Process *zombie = make_zombie_child(live);
    ASSERT_TRUE(proc_reparent_children(live) == init);
    ASSERT_TRUE(zombie->parent == init);
    ASSERT_TRUE(proc_find_zombie_child(init) != NULL);
    return 0;
}

static int test_reparent_when_init_exits(void) {
    reset_proc_state();
    proc_init();
    Process *init = make_parent();
    proc_set_init(init);
    Process *c = make_zombie_child(init);
    /* init's own children get no parent, and nobody is woken */
    ASSERT_TRUE(proc_reparent_children(init) == NULL);
    ASSERT_TRUE(c->parent == NULL);
    return 0;
}

/* Fork failing after the PCB is linked must undo all of it */
static int test_fork_failure_unlinks_and_frees(void) {
    reset_proc_state();
    proc_init();
    Process *parent = make_parent();
    Process *head = proc_list;
    int live_before = kmalloc_call_count - kfree_call_count;
    thread_create_force_fail = 1;
    ForkContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_TRUE(proc_fork(parent, &ctx) == NULL);
    ASSERT_TRUE(proc_list == head && list_well_formed());
    ASSERT_EQ(vmm_free_calls, 1);                 /* copied pages too */
    ASSERT_EQ(release_fds_calls, 1);              /* pipe refs fd_table_dup took */
    ASSERT_EQ(kmalloc_call_count - kfree_call_count, live_before);
    return 0;
}

/* --- Test suite export --- */

/* fork: the child's thread starts with the parent's FPU/SSE registers and FS base */
static int test_proc_fork_copies_fpu_and_fs_base(void) {
    reset_proc_state();
    proc_init();
    fpu_save_calls = 0;
    fpu_save_last = NULL;

    Process *parent = proc_create((thread_entry_t)0xDEAD, NULL);
    ASSERT_TRUE(parent != NULL);
    parent->page_table = 0x300000;
    static uint8_t parent_area[512];
    parent->main_thread->fpu_state = parent_area;
    parent->main_thread->fs_base = 0x7000123450ULL;
    test_current_thread = parent->main_thread;

    ForkContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    Process *child = proc_fork(parent, &ctx);
    ASSERT_TRUE(child != NULL);
    Thread *ct = child->main_thread;
    ASSERT_TRUE(ct != NULL && ct != parent->main_thread);
    ASSERT_EQ(ct->fs_base, 0x7000123450ULL);
    ASSERT_EQ(fpu_save_calls, 1);
    ASSERT_TRUE(fpu_save_last == ct->fpu_state);
    return 0;
}

TestCase process_tests[] = {
    {"fork_copies_fpu_and_fs_base", test_proc_fork_copies_fpu_and_fs_base},
    {"init_creates_boot_process", test_proc_init_creates_boot_process},
    {"init_pid_sequence", test_proc_init_pid_sequence},
    {"create_basic", test_proc_create_basic},
    {"create_links_in_list", test_proc_create_links_in_list},
    {"create_thread_failure", test_proc_create_thread_failure},
    {"create_kmalloc_failure", test_proc_create_kmalloc_failure},
    {"current_returns_boot", test_proc_current_returns_boot},
    {"create_pid_sequence", test_proc_create_pid_sequence},
    {"threads_point_at_process", test_threads_point_at_process},
    {"current_with_large_tid", test_current_with_large_tid},
    {"reap_frees_everything", test_reap_frees_everything},
    {"reap_unlinks_head_middle_tail", test_reap_unlinks_head_middle_tail},
    {"reap_rejects_live_child", test_reap_rejects_live_child},
    {"reparent_to_init", test_reparent_to_init},
    {"reparent_when_init_exits", test_reparent_when_init_exits},
    {"fork_failure_unlinks_and_frees", test_fork_failure_unlinks_and_frees},
    {"state_alive_to_zombie", test_state_alive_to_zombie},
    {"state_values_distinct", test_state_values_distinct},
    {"max_processes_boundary", test_max_processes_boundary},
    {"parent_null_by_default", test_parent_null_by_default},
    {"current_after_thread_switch", test_current_after_thread_switch},
};

int process_test_count = sizeof(process_tests) / sizeof(process_tests[0]);
