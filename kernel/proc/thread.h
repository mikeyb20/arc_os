#ifndef ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_THREAD_H

#include <stdint.h>
#include <stddef.h>

/* Thread ID type */
typedef uint32_t tid_t;

/* Thread states */
#define THREAD_CREATED  0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_DEAD     4
#define THREAD_STOPPED  5

/* Kernel stack: one order-2 buddy block, 16 KB (M1-02b) */
#define THREAD_STACK_ORDER 2
#define THREAD_STACK_SIZE  (4096 << THREAD_STACK_ORDER)

/* Thread entry function type */
typedef void (*thread_entry_t)(void *arg);

/* Callee-saved registers + RSP — saved/restored by context_switch */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsp;
} ThreadContext;

struct Process;  /* proc/process.h */

/* Thread Control Block */
typedef struct Thread {
    tid_t           tid;
    uint8_t         state;
    ThreadContext   context;
    uint8_t *stack_base;     /* Kernel stack, an alloc_pages block in the HHDM (NULL for boot thread) */
    size_t          stack_size;
    uint64_t        kernel_stack_top; /* Top of kernel stack for TSS.rsp0 / SYSCALL */
    thread_entry_t  entry;
    void           *arg;
    void *fpu_state;      /* XSAVE area, 64-byte aligned (fpu.h); NULL for the boot thread */
    uint64_t        fs_base;        /* User TLS pointer (arch_prctl ARCH_SET_FS) */
    uint64_t        clear_child_tid; /* set_tid_address pointer; used by M1-16 thread exit */
    struct Process *proc;           /* Owning process; NULL for a thread with none (idle) */
    struct Thread  *next;           /* Intrusive list for scheduler */
} Thread;

/* Initialize threading — creates TCB for boot thread (tid=0). */
void thread_init(void);

/* Create a new kernel thread. Returns NULL on failure. */
Thread *thread_create(thread_entry_t entry, void *arg);

/* Destroy a dead thread: frees its kernel stack, FPU area and TCB. The thread
 * must be DEAD and must have switched away for the last time (never itself). */
void thread_destroy(Thread *t);

/* Get the thread running on this CPU (one GS-relative load, M1-03). */
Thread *thread_current(void);

/* Set the thread running on this CPU (used by scheduler). */
void thread_set_current(Thread *t);

/* Assembly context switch: saves old context, loads new context, returns. */
extern void context_switch(ThreadContext *old, ThreadContext *new_ctx);

#endif /* ARCHOS_PROC_THREAD_H */
