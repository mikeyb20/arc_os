#include "proc/thread.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/mem.h"

static Thread *current_thread = NULL;
static tid_t next_tid = 0;

/* Trampoline: first thing a new thread executes after context_switch returns.
 * Enables interrupts, calls the entry function, marks thread DEAD, then yields. */
static void thread_trampoline(void) {
    Thread *t = thread_current();
    __asm__ volatile ("sti");
    t->entry(t->arg);
    t->state = THREAD_DEAD;
    /* Yield to scheduler — thread will never be scheduled again */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void thread_init(void) {
    /* Create TCB for the boot thread (already running) */
    Thread *boot = kmalloc(sizeof(Thread), GFP_ZERO);
    if (boot == NULL) {
        kprintf("[PROC] FATAL: cannot allocate boot thread TCB\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    boot->tid = next_tid++;
    boot->state = THREAD_RUNNING;
    boot->stack_base = NULL;    /* Boot thread uses the original kernel stack */
    boot->stack_size = 0;
    boot->entry = NULL;
    boot->arg = NULL;
    boot->next = NULL;

    current_thread = boot;
    kprintf("[PROC] Threading initialized (boot thread tid=%u)\n", boot->tid);
}

Thread *thread_create(thread_entry_t entry, void *arg) {
    Thread *t = kmalloc(sizeof(Thread), GFP_ZERO);
    if (t == NULL) return NULL;

    /* Allocate kernel stack */
    t->stack_base = kmalloc(THREAD_STACK_SIZE, 0);
    if (t->stack_base == NULL) {
        kfree(t);
        return NULL;
    }

    t->tid = next_tid++;
    t->state = THREAD_READY;
    t->stack_size = THREAD_STACK_SIZE;
    t->entry = entry;
    t->arg = arg;
    t->next = NULL;

    /* Set up initial stack so context_switch's ret jumps to thread_trampoline.
     * Stack grows downward, so top = base + size.
     * We place the trampoline address where ret will pop it. */
    uint64_t *stack_top = (uint64_t *)(t->stack_base + THREAD_STACK_SIZE);

    /* Push fake return address for context_switch's ret */
    stack_top[-1] = (uint64_t)thread_trampoline;

    /* RSP points to the return address (what ret will pop) */
    t->context.rsp = (uint64_t)&stack_top[-1];

    /* Zero callee-saved registers for clean start */
    t->context.r15 = 0;
    t->context.r14 = 0;
    t->context.r13 = 0;
    t->context.r12 = 0;
    t->context.rbx = 0;
    t->context.rbp = 0;

    kprintf("[PROC] Created thread tid=%u\n", t->tid);
    return t;
}

void thread_destroy(Thread *t) {
    if (t == NULL) return;
    if (t->stack_base != NULL) {
        kfree(t->stack_base);
    }
    kfree(t);
}

Thread *thread_current(void) {
    return current_thread;
}

void thread_set_current(Thread *t) {
    current_thread = t;
}
