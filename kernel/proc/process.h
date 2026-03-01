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

/* Process Control Block */
typedef struct Process {
    pid_t           pid;
    uint8_t         state;
    Thread         *main_thread;
    uint64_t        page_table;     /* PML4 phys addr (all share kernel PML4 for now) */
    struct Process *parent;
    struct Process *next;           /* Process list linkage */
} Process;

/* Initialize process management — creates process 0 for the boot thread. */
void proc_init(void);

/* Create a new process with a kernel thread running entry(arg). Returns NULL on failure. */
Process *proc_create(thread_entry_t entry, void *arg);

/* Get the process for the current thread. */
Process *proc_current(void);

#endif /* ARCHOS_PROC_PROCESS_H */
