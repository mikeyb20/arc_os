#include "proc/process.h"
#include "proc/sched.h"
#include "proc/fd.h"
#include "proc/proc_syscalls.h"
#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "mm/mm.h"
#include "arch/hal.h"
#include "lib/kprintf.h"
#include "lib/string.h"

/* Every unreaped process, newest first; doubly linked so reaping unlinks in O(1) */
static Process *proc_list = NULL;
/* PIDs only count up (2^32 before a wrap), so a freed PID never matches a stale
 * pid or pgid held elsewhere (the tty's foreground group) */
static pid_t next_pid = 0;
/* Adopts orphans (proc_reparent_children); NULL until init exists */
static Process *init_proc = NULL;

static void proc_unlink(Process *p) {
    if (p->prev != NULL)
        p->prev->next = p->next;
    else
        proc_list = p->next;
    if (p->next != NULL) p->next->prev = p->prev;
    p->next = p->prev = NULL;
}

/* Common PCB setup: assign PID, set state, init signals, link into process list. */
static void proc_setup(Process *p) {
    p->pid = next_pid++;
    p->pgid = p->pid;  /* Each process starts as its own group leader */
    p->state = PROC_ALIVE;
    p->umask = 022;     /* Default: owner full, group/other no write */
    p->cwd = NULL;   /* "/" */
    sig_init(&p->sig);
    wq_init(&p->child_exit_wq);
    p->prev = NULL;
    p->next = proc_list;
    if (proc_list != NULL) proc_list->prev = p;
    proc_list = p;
}

void proc_init(void) {
    Process *p = kmalloc(sizeof(Process), GFP_ZERO);
    if (p == NULL) {
        kprintf("[PROC] FATAL: cannot allocate boot process PCB\n");
        KERNEL_PANIC();
    }

    proc_setup(p);
    proc_set_main_thread(p, thread_current());

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
    proc_set_main_thread(p, t);   /* p->mm stays NULL: kernel page tables only */

    sched_add_thread(t);

    kdebug("[PROC] Created process pid=%u (thread tid=%u)\n", p->pid, t->tid);
    return p;
}

Process *proc_create_user(void) {
    Process *p = kmalloc(sizeof(Process), GFP_ZERO);
    if (p == NULL) return NULL;

    p->mm = mm_create();
    if (p->mm == NULL) {
        kfree(p);
        return NULL;
    }
    proc_setup(p);

    kdebug("[PROC] Created user process pid=%u (pml4=0x%lx)\n", p->pid, p->mm->pgd);
    return p;
}

Process *proc_current(void) {
    Thread *t = thread_current();
    return t != NULL ? t->proc : NULL;
}

Process *proc_get_by_pid(uint32_t pid) {
    for (Process *p = proc_list; p != NULL; p = p->next) {
        if (p->pid == pid) {
            return p;
        }
    }
    return NULL;
}

void proc_set_main_thread(Process *p, Thread *t) {
    p->main_thread = t;
    t->proc = p;
}

void proc_set_init(Process *p) {
    init_proc = p;
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

    /* This CPU's entry stacks (TSS.RSP0 and SYSCALL) for this thread */
    Thread *t = thread_current();
    hal_set_kernel_stack(t->kernel_stack_top);

    /* Switch to child's address space */
    mm_switch(child->mm);

    /* Return to user mode with RAX=0, restoring callee-saved registers */
    fork_return_to_user(&args->ctx);
}

Process *proc_fork(Process *parent, const ForkContext *user_ctx) {
    if (parent == NULL || parent->mm == NULL) return NULL;

    /* 1. Fork address space */
    Mm *child_mm = mm_fork(parent->mm);
    if (child_mm == NULL) return NULL;

    /* 2. Create child process PCB */
    Process *child = kmalloc(sizeof(Process), GFP_ZERO);
    if (child == NULL) {
        mm_put(child_mm);
        return NULL;
    }
    proc_setup(child);
    child->mm = child_mm;
    vfs_cwd_copy(child, parent);
    child->uid  = parent->uid;
    child->gid  = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->umask = parent->umask;
    child->pgid = parent->pgid;
    child->parent = parent;
    sig_fork_copy(&child->sig, &parent->sig);   /* actions and mask, nothing pending */

    /* 3. Duplicate FD table */
    child->fd_table = parent->fd_table != NULL ? fd_table_dup(parent->fd_table) : NULL;
    int fds_ok = parent->fd_table == NULL || child->fd_table != NULL;

    /* 4. Create child's kernel thread; it frees args when it starts */
    ForkChildArgs *args = fds_ok ? kmalloc(sizeof(ForkChildArgs), 0) : NULL;
    Thread *t = NULL;
    if (args != NULL) {
        args->child = child;
        args->ctx = *user_ctx;
        t = thread_create(fork_child_entry, args);
    }
    if (t == NULL) {
        /* Undo everything above: the copied pages (not just the tables), the
         * File references fd_table_dup took, and the list entry */
        kfree(args);
        proc_release_fds(child);
        vfs_cwd_release(child);
        mm_put(child_mm);
        proc_unlink(child);
        kfree(child);
        return NULL;
    }
    /* The child resumes with the parent's user FPU/SSE registers (live now:
     * the kernel never touches them) and TLS pointer */
    fpu_save_current(t->fpu_state);
    t->fs_base = thread_current()->fs_base;
    t->affinity = thread_current()->affinity;   /* Linux: fork keeps the CPU mask */

    proc_set_main_thread(child, t);
    sched_add_thread(t);

    kdebug("[PROC] Forked pid=%u -> pid=%u\n", parent->pid, child->pid);
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
        if (p->parent == parent) {
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
    int pid = (int)child->pid;

    /* The address space went at exit (proc_exit). The child's thread is DEAD;
     * it may still be finishing its last switch on another CPU, so wait until
     * it is off that CPU before freeing its stack. Nothing else holds the
     * Process: wait queues hold only running threads' sleepers, signals and
     * the tty go by pid/pgid, and PIDs are not reused. */
    sched_wait_off_cpu(child->main_thread);
    proc_unlink(child);
    thread_destroy(child->main_thread);
    kfree(child);
    return pid;
}

Process *proc_reparent_children(Process *p) {
    Process *heir = (p == init_proc) ? NULL : init_proc;
    int zombie = 0;
    for (Process *c = proc_list; c != NULL; c = c->next) {
        if (c->parent != p) continue;
        c->parent = heir;
        if (c->state == PROC_ZOMBIE) zombie = 1;
    }
    return (zombie && heir != NULL) ? heir : NULL;
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
    if (child->parent != parent) return 0;
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
        cb(p, ctx);
        count++;
    }
    return count;
}
