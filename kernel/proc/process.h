#ifndef ARCHOS_PROC_PROCESS_H
#define ARCHOS_PROC_PROCESS_H

#include "proc/thread.h"
#include <stdint.h>

/* Process ID type */
typedef uint32_t pid_t;

/* Process states */
#define PROC_ALIVE       0
#define PROC_ZOMBIE      1
#define PROC_TERMINATED  2

/* Forward declaration */
typedef struct FdTable FdTable;

/* Process Control Block */
typedef struct Process {
    pid_t           pid;
    uint8_t         state;
    Thread         *main_thread;
    uint64_t        page_table;     /* PML4 phys addr (0 = use kernel PML4) */
    FdTable        *fd_table;       /* Per-process file descriptor table */
    uint64_t        brk_current;    /* Current program break */
    uint64_t        brk_start;      /* Initial program break */
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

/* Register a thread as the main thread of a process. */
void proc_set_main_thread(Process *p, Thread *t);

#endif /* ARCHOS_PROC_PROCESS_H */
