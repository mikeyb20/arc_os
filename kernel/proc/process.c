#include "proc/process.h"
#include "proc/sched.h"
#include "proc/fd.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "arch/x86_64/usermode.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/syscall.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/fpu.h"
#include "lib/kprintf.h"
#include "lib/string.h"

static Process *proc_list = NULL;
static pid_t next_pid = 0;

/* Simple mapping: each thread's tid maps 1:1 to a process for now.
 * We store a static table indexed by tid for quick lookup. */
#define MAX_PROCESSES 64
static Process *proc_table[MAX_PROCESSES];

/* Common PCB setup: assign PID, set state, init signals, link into process list. */
static void proc_setup(Process *p) {
    p->pid = next_pid++;
    p->pgid = p->pid;  /* Each process starts as its own group leader */
    p->state = PROC_ALIVE;
    p->umask = 022;     /* Default: owner full, group/other no write */
    strncpy(p->cwd, "/", PATH_MAX);
    sig_init(&p->sig);
    wq_init(&p->child_exit_wq);
    p->next = proc_list;
    proc_list = p;
}

void proc_init(void) {
    Process *p = kmalloc(sizeof(Process), GFP_ZERO);
    if (p == NULL) {
        kprintf("[PROC] FATAL: cannot allocate boot process PCB\n");
        KERNEL_PANIC();
    }

    proc_setup(p);
    p->main_thread = thread_current();
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

    proc_setup(p);
    p->main_thread = t;
    p->page_table = 0;  /* Share kernel page tables for now */

    if (t->tid < MAX_PROCESSES) {
        proc_table[t->tid] = p;
    }

    sched_add_thread(t);

    kprintf("[PROC] Created process pid=%u (thread tid=%u)\n", p->pid, t->tid);
    return p;
}

Process *proc_create_user(void) {
    Process *p = kmalloc(sizeof(Process), GFP_ZERO);
    if (p == NULL) return NULL;

    proc_setup(p);
    p->page_table = vmm_create_user_pml4();

    kprintf("[PROC] Created user process pid=%u (pml4=0x%lx)\n", p->pid, p->page_table);
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

Process *proc_get_by_tid(uint32_t tid) {
    if (tid < MAX_PROCESSES) {
        return proc_table[tid];
    }
    return NULL;
}

Process *proc_get_by_pid(uint32_t pid) {
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (p->pid == pid && p->state != PROC_TERMINATED) {
            return p;
        }
    }
    return NULL;
}

void proc_set_main_thread(Process *p, Thread *t) {
    p->main_thread = t;
    if (t->tid < MAX_PROCESSES) {
        proc_table[t->tid] = p;
    }
}

/* --- Fork support --- */

/* Data passed to the fork child's kernel thread */
typedef struct {
    Process    *child;
    ForkContext ctx;
} ForkChildArgs;

/* Kernel thread entry for the forked child. args is this child's own kmalloc'd
 * copy (a shared global was overwritten when a parent forked twice before the
 * first child ran: both children then started with the second fork's context). */
static void fork_child_entry(void *arg) {
    ForkChildArgs *heap_args = (ForkChildArgs *)arg;
    ForkChildArgs local = *heap_args;
    kfree(heap_args);
    ForkChildArgs *args = &local;
    Process *child = args->child;

    /* Set TSS.rsp0 and syscall kernel RSP for this thread */
    Thread *t = thread_current();
    gdt_set_kernel_stack(t->kernel_stack_top);
    syscall_kernel_rsp = t->kernel_stack_top;

    /* Switch to child's address space */
    paging_write_cr3(child->page_table);

    /* Return to user mode with RAX=0, restoring callee-saved registers */
    fork_return_to_user(&args->ctx);
}

Process *proc_fork(Process *parent, const ForkContext *user_ctx) {
    if (parent == NULL || parent->page_table == 0) return NULL;

    /* 1. Fork address space */
    uint64_t child_pml4 = vmm_fork_address_space(parent->page_table);
    if (child_pml4 == 0) return NULL;

    /* 2. Create child process PCB */
    Process *child = kmalloc(sizeof(Process), GFP_ZERO);
    if (child == NULL) {
        vmm_destroy_user_pml4(child_pml4);
        return NULL;
    }
    proc_setup(child);
    child->page_table = child_pml4;
    child->brk_start = parent->brk_start;
    child->brk_current = parent->brk_current;
    strncpy(child->cwd, parent->cwd, PATH_MAX);
    child->uid  = parent->uid;
    child->gid  = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->umask = parent->umask;
    child->pgid = parent->pgid;
    child->parent = parent;

    /* 3. Duplicate FD table */
    child->fd_table = fd_table_dup(parent->fd_table);

    /* 4. Create child's kernel thread; it frees args when it starts */
    ForkChildArgs *args = kmalloc(sizeof(ForkChildArgs), 0);
    Thread *t = NULL;
    if (args != NULL) {
        args->child = child;
        args->ctx = *user_ctx;
        t = thread_create(fork_child_entry, args);
    }
    if (t == NULL) {
        kfree(args);
        kfree(child->fd_table);
        vmm_destroy_user_pml4(child_pml4);
        kfree(child);
        return NULL;
    }
    /* The child resumes with the parent's user FPU/SSE registers (live now:
     * the kernel never touches them) and TLS pointer */
    fpu_save_current(t->fpu_state);
    t->fs_base = thread_current()->fs_base;

    proc_set_main_thread(child, t);
    sched_add_thread(t);

    kprintf("[PROC] Forked pid=%u -> pid=%u\n", parent->pid, child->pid);
    return child;
}

/* --- Zombie/reap helpers --- */

Process *proc_find_zombie_child(Process *parent) {
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (p->parent == parent && p->state == PROC_ZOMBIE) {
            return p;
        }
    }
    return NULL;
}

int proc_has_children(Process *parent) {
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (p->parent == parent && p->state != PROC_TERMINATED) {
            return 1;
        }
    }
    return 0;
}

int proc_reap(Process *child, int32_t *status_out) {
    if (child == NULL || child->state != PROC_ZOMBIE) return -EINVAL;
    if (status_out != NULL) {
        *status_out = child->exit_status;
    }
    child->state = PROC_TERMINATED;
    return (int)child->pid;
}

Process *proc_find_stopped_child(Process *parent) {
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (p->parent == parent && p->state == PROC_STOPPED
            && p->exit_status != 0) {
            return p;
        }
    }
    return NULL;
}

static int proc_wait_selects(const Process *parent, const Process *child, int32_t pid) {
    if (child->parent != parent || child->state == PROC_TERMINATED) return 0;
    if (pid > 0)   return child->pid == (pid_t)pid;
    if (pid == -1) return 1;
    if (pid == 0)  return child->pgid == parent->pgid;
    return child->pgid == (pid_t)(-pid);
}

Process *proc_find_waitable(Process *parent, int32_t pid, int want_stopped) {
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (!proc_wait_selects(parent, p, pid)) continue;
        if (p->state == PROC_ZOMBIE) return p;
        if (want_stopped && p->state == PROC_STOPPED && p->exit_status != 0) return p;
    }
    return NULL;
}

int proc_has_selected_child(Process *parent, int32_t pid) {
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (proc_wait_selects(parent, p, pid)) return 1;
    }
    return 0;
}

int proc_foreach(void (*cb)(Process *p, void *ctx), void *ctx) {
    int count = 0;
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (p->state != PROC_TERMINATED) {
            cb(p, ctx);
            count++;
        }
    }
    return count;
}
