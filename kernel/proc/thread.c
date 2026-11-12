#include "proc/thread.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "proc/sched.h"
#include "arch/hal.h"

static tid_t next_tid = 0;

/* Trampoline: first thing a new thread executes after context_switch returns.
 * Finishes the switch that started it (releases the run-queue lock), enables
 * interrupts, calls the entry function; if that returns, the thread is DEAD
 * and switches away for good (no one frees a kernel thread without a process). */
static void thread_trampoline(void) {
    sched_finish_switch();
    Thread *t = thread_current();
    hal_enable_interrupts();
    t->entry(t->arg);
    (void)irq_save();
    t->state = THREAD_DEAD;
    sched_schedule();
    for (;;) hal_halt_forever();
}

void thread_init(void) {
    /* Create TCB for the boot thread (already running) */
    Thread *boot = kmalloc(sizeof(Thread), GFP_ZERO);
    if (boot == NULL) {
        kprintf("[PROC] FATAL: cannot allocate boot thread TCB\n");
        KERNEL_PANIC();
    }

    boot->tid = next_tid++;
    boot->state = THREAD_RUNNING;
    cpumask_fill(&boot->affinity);

    thread_set_current(boot);   /* kept per CPU, through GS (M1-03) */
    kprintf("[PROC] Threading initialized (boot thread tid=%u)\n", boot->tid);
}

Thread *thread_create(thread_entry_t entry, void *arg) {
    Thread *t = kmalloc(sizeof(Thread), GFP_ZERO);
    if (t == NULL) return NULL;

    /* Allocate kernel stack */
    /* Kernel stack: one order-2 buddy block, no guard page yet (M1-16) */
    struct page *stack = alloc_pages(THREAD_STACK_ORDER, GFP_KERNEL);
    if (stack == NULL) {
        kfree(t);
        return NULL;
    }
    t->stack_base = page_to_virt(stack);

    /* Every thread may enter user mode: give it a clean FPU/SSE image */
    t->fpu_state = fpu_state_alloc();
    if (t->fpu_state == NULL) {
        free_pages(virt_to_page(t->stack_base), THREAD_STACK_ORDER);
        kfree(t);
        return NULL;
    }

    t->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    t->state = THREAD_READY;
    t->cpu = hal_cpu_index();   /* where a first wakeup looks first */
    cpumask_fill(&t->affinity);
    t->stack_size = THREAD_STACK_SIZE;
    t->kernel_stack_top = (uint64_t)(t->stack_base + THREAD_STACK_SIZE);
    t->entry = entry;
    t->arg = arg;

    /* Set up initial stack so context_switch's ret jumps to thread_trampoline.
     * Stack grows downward, so top = base + size.
     * We place the trampoline address where ret will pop it. */
    uint64_t *stack_top = (uint64_t *)(t->stack_base + THREAD_STACK_SIZE);

    /* Push fake return address for context_switch's ret */
    stack_top[-1] = (uint64_t)thread_trampoline;

    /* RSP points to the return address (what ret will pop) */
    t->context.rsp = (uint64_t)&stack_top[-1];

    kdebug("[PROC] Created thread tid=%u\n", t->tid);
    return t;
}

void thread_destroy(Thread *t) {
    if (t == NULL) return;
    if (t->stack_base != NULL) {
        free_pages(virt_to_page(t->stack_base), THREAD_STACK_ORDER);
    }
    fpu_state_free(t->fpu_state);
    kfree(t);
}

Thread *thread_current(void) {
    return hal_current_thread();
}

void thread_set_current(Thread *t) {
    hal_set_current_thread(t);
}
