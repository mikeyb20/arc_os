#include "proc/init.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/kernel_lock.h"
#include "proc/elf.h"
#include "proc/fd.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "arch/hal.h"
#include "lib/kprintf.h"
#include "lib/testmode.h"
#include "proc/proc_syscalls.h"
#include "proc/exec_stack.h"

/* Argument passed to the init kernel thread */
typedef struct {
    const char *path;      /* module path (its copy in the root ramfs): argv[0], AT_EXECFN */
    Process    *proc;
} InitArgs;

static InitArgs g_init_args;

/* Kernel-thread entry: loads ELF, sets up user stack, transitions to user mode */
static void init_thread_entry(void *arg) {
    InitArgs *args = (InitArgs *)arg;
    Process *p = args->proc;

    /* Like a syscall: interrupts off and the kernel lock until user mode
     * (proc_enter_user releases it) */
    (void)irq_save();
    kernel_lock();

    kprintf("[INIT] Loading init ELF '%s'\n", args->path);

    /* Load it like execve does: from the module's copy in the root ramfs,
     * through the page cache (the boot module memory is not used) */
    ElfLoadResult result;
    int err = exec_load(NULL, args->path, p->mm, &result, NULL);
    if (err != 0) {
        kprintf("[INIT] FATAL: exec_load failed (%d)\n", err);
        return;
    }

    /* Stack VMA (pages come on first touch) and program break */
    if (mm_setup_stack(p->mm, result.stack_exec, result.brk_start) != 0) {
        kprintf("[INIT] FATAL: out of memory for user stack\n");
        return;
    }
    kprintf("[INIT] User stack: 0x%lx - 0x%lx, grows to %lu KiB\n", (uint64_t)(USER_STACK_TOP - MM_STACK_INIT),
            (uint64_t)USER_STACK_TOP, (uint64_t)(MM_STACK_MAX / 1024));

    /* argv = { path }, envp = { HOME=/, TERM=linux } as Linux gives init */
    ExecStrings *strings = kmalloc(sizeof(ExecStrings), 0);
    uint8_t *stack_buf = kmalloc(EXEC_STACK_BUF, 0);
    if (strings == NULL || stack_buf == NULL) {
        kprintf("[INIT] FATAL: out of memory for the initial stack\n");
        return;
    }
    exec_strings_init(strings);
    if (exec_strings_add(strings, args->path, 0) != 0 ||
        exec_strings_add(strings, "HOME=/", 1) != 0 ||
        exec_strings_add(strings, "TERM=linux", 1) != 0) {
        kprintf("[INIT] FATAL: init arguments too long\n");
        return;
    }

    /* This CPU's entry stacks (TSS.RSP0 and SYSCALL) */
    Thread *t = thread_current();
    hal_set_kernel_stack(t->kernel_stack_top);

    /* Switch to the process's address space */
    mm_switch(p->mm);

    kprintf("[INIT] Jumping to user mode: entry=0x%lx\n", result.entry_point);

    /* System V stack, clean CPU state, user mode — does not return */
    proc_enter_user(p, &result, strings, args->path, stack_buf);
}

int init_launch(const BootInfo *info) {
    /* Find the init module */
    const BootModule *init_mod = NULL;
    for (uint64_t i = 0; i < info->module_count; i++) {
        kprintf("[INIT] Module %lu: path='%s' size=%lu addr=%p\n",
                i, info->modules[i].path, info->modules[i].size,
                info->modules[i].address);
        if (init_mod == NULL) {
            init_mod = &info->modules[i];
        }
    }

    if (init_mod == NULL) {
        kprintf("[INIT] No boot modules found — cannot launch init\n");
        return -1;
    }

    kprintf("[INIT] Using module '%s' as init (%lu bytes)\n",
            init_mod->path, init_mod->size);

    /* Create user process with its own address space */
    Process *p = proc_create_user();
    if (p == NULL) {
        kprintf("[INIT] FATAL: proc_create_user failed\n");
        return -1;
    }

    p->fd_table = fd_table_new();
    if (p->fd_table == NULL) {
        kprintf("[INIT] FATAL: cannot allocate fd table\n");
        return -1;
    }

    /* fds 0, 1, 2 = one open /dev/tty, inherited by every process (Linux
     * programs expect them open). Without them a process's first open()
     * returned 0-2, so opendir() in the shell took fd 1 and printf output went
     * to a directory. */
    File *tty = NULL;
    if (vfs_open("/dev/tty", O_RDWR, 0, &tty) == 0) {
        for (int fd = 0; fd < 3; fd++) {
            File *f = fd == 0 ? tty : fget_ref(tty);
            if (fd_install_at(p->fd_table, fd, f, 0) != 0) fput(f);
        }
    } else {
        kprintf("[INIT] WARNING: no /dev/tty (fds 0-2 fall back to the console)\n");
    }

    /* Create kernel thread that will load ELF and transition to user mode */
    g_init_args.path = init_mod->path;
    g_init_args.proc = p;

    Thread *t = thread_create(init_thread_entry, &g_init_args);
    if (t == NULL) {
        kprintf("[INIT] FATAL: thread_create failed\n");
        return -1;
    }

    /* Register thread with process */
    proc_set_main_thread(p, t);
    proc_set_init(p);
    testmode_set_init_pid((int)p->pid);

    /* Add to scheduler */
    sched_add_thread(t);

    kprintf("[INIT] Init process pid=%u scheduled (tid=%u)\n", p->pid, t->tid);
    return 0;
}
