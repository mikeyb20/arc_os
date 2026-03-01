#include "proc/process.h"
#include "proc/sched.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"

static Process *proc_list = NULL;
static pid_t next_pid = 0;

/* Simple mapping: each thread's tid maps 1:1 to a process for now.
 * We store a static table indexed by tid for quick lookup. */
#define MAX_PROCESSES 64
static Process *proc_table[MAX_PROCESSES];

void proc_init(void) {
    Process *p = kmalloc(sizeof(Process), GFP_ZERO);
    if (p == NULL) {
        kprintf("[PROC] FATAL: cannot allocate boot process PCB\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    p->pid = next_pid++;
    p->state = PROC_ALIVE;
    p->main_thread = thread_current();
    p->page_table = 0;  /* Uses current kernel page tables */
    p->parent = NULL;
    p->next = NULL;

    proc_list = p;
    if (p->main_thread->tid < MAX_PROCESSES) {
        proc_table[p->main_thread->tid] = p;
    }

    kprintf("[PROC] Process management initialized (boot process pid=%u)\n", p->pid);
}

Process *proc_create(thread_entry_t entry, void *arg) {
    Process *p = kmalloc(sizeof(Process), GFP_ZERO);
    if (p == NULL) return NULL;

    Thread *t = thread_create(entry, arg);
    if (t == NULL) {
        kfree(p);
        return NULL;
    }

    p->pid = next_pid++;
    p->state = PROC_ALIVE;
    p->main_thread = t;
    p->page_table = 0;  /* Share kernel page tables for now */
    p->parent = NULL;
    p->next = proc_list;
    proc_list = p;

    if (t->tid < MAX_PROCESSES) {
        proc_table[t->tid] = p;
    }

    sched_add_thread(t);

    kprintf("[PROC] Created process pid=%u (thread tid=%u)\n", p->pid, t->tid);
    return p;
}

Process *proc_current(void) {
    Thread *t = thread_current();
    if (t == NULL) return NULL;
    if (t->tid < MAX_PROCESSES) {
        return proc_table[t->tid];
    }
    return NULL;
}
