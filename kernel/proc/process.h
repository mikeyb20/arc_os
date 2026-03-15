#ifndef ARCHOS_PROC_PROCESS_H
#define ARCHOS_PROC_PROCESS_H

#include "proc/thread.h"
#include "proc/signal.h"
#include "proc/waitqueue.h"
#include <stdint.h>

/* Process ID type */
typedef uint32_t pid_t;

/* Process states */
#define PROC_ALIVE       0
#define PROC_ZOMBIE      1
#define PROC_TERMINATED  2

/* Saved user context for fork (captured from SYSCALL frame) */
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

/* Forward declaration */
typedef struct FdTable FdTable;

/* Process Control Block */
typedef struct Process {
    pid_t           pid;
    uint8_t         state;
    int32_t         exit_status;    /* Exit status for wait() */
    Thread         *main_thread;
    uint64_t        page_table;     /* PML4 phys addr (0 = use kernel PML4) */
    FdTable        *fd_table;       /* Per-process file descriptor table */
    uint64_t        brk_current;    /* Current program break */
    uint64_t        brk_start;      /* Initial program break */
    SigState        sig;            /* Per-process signal state */
    WaitQueue       child_exit_wq;  /* Parents sleep here in sys_wait */
    struct Process *parent;
    struct Process *next;           /* Process list linkage */
} Process;

/* Initialize process management — creates process 0 for the boot thread. */
void proc_init(void);

/* Create a new process with a kernel thread running entry(arg). Returns NULL on failure. */
Process *proc_create(thread_entry_t entry, void *arg);

/* Create a user-space process with its own address space. Returns NULL on failure. */
Process *proc_create_user(void);

/* Get the process for the current thread. */
Process *proc_current(void);

/* Look up process by thread ID. Returns NULL if not found. */
Process *proc_get_by_tid(uint32_t tid);

/* Look up process by PID. Returns NULL if not found or terminated. */
Process *proc_get_by_pid(uint32_t pid);

/* Register a thread as the main thread of a process. */
void proc_set_main_thread(Process *p, Thread *t);

/* Fork the current process. Returns child Process* or NULL on failure.
 * The child's thread will return to user_ctx location with RAX=0. */
Process *proc_fork(Process *parent, const ForkContext *user_ctx);

/* Find a zombie child of the given parent. Returns NULL if none. */
Process *proc_find_zombie_child(Process *parent);

/* Reap a zombie child: copy exit status, mark TERMINATED. */
int proc_reap(Process *child, int32_t *status_out);

/* Check if a process has any live (non-TERMINATED) children. */
int proc_has_children(Process *parent);

#endif /* ARCHOS_PROC_PROCESS_H */
