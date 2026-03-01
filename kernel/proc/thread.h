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

/* Default kernel stack size: 16 KB */
#define THREAD_STACK_SIZE  (16 * 1024)

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

/* Thread Control Block */
typedef struct Thread {
    tid_t           tid;
    uint8_t         state;
    ThreadContext   context;
    uint8_t        *stack_base;     /* Heap-allocated kernel stack (NULL for boot thread) */
    size_t          stack_size;
    thread_entry_t  entry;
    void           *arg;
    struct Thread  *next;           /* Intrusive list for scheduler */
} Thread;

/* Initialize threading — creates TCB for boot thread (tid=0). */
void thread_init(void);

/* Create a new kernel thread. Returns NULL on failure. */
Thread *thread_create(thread_entry_t entry, void *arg);

/* Destroy a dead thread — frees stack and TCB. Thread must be DEAD. */
void thread_destroy(Thread *t);

/* Get the currently running thread. */
Thread *thread_current(void);

/* Set the currently running thread (used by scheduler). */
void thread_set_current(Thread *t);

/* Assembly context switch: saves old context, loads new context, returns. */
extern void context_switch(ThreadContext *old, ThreadContext *new_ctx);

#endif /* ARCHOS_PROC_THREAD_H */
