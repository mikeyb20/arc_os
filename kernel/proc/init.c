#include "proc/init.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/elf.h"
#include "proc/fd.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/syscall.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/usermode.h"
#include "lib/kprintf.h"
#include "lib/testmode.h"
#include "proc/proc_syscalls.h"
#include "proc/exec_stack.h"

/* Argument passed to the init kernel thread */
typedef struct {
    const void *elf_data;
    uint64_t    elf_size;
    const char *path;      /* module path: init's argv[0] and AT_EXECFN */
    Process    *proc;
} InitArgs;

static InitArgs g_init_args;

/* Kernel-thread entry: loads ELF, sets up user stack, transitions to user mode */
static void init_thread_entry(void *arg) {
    InitArgs *args = (InitArgs *)arg;
    Process *p = args->proc;

    kprintf("[INIT] Loading init ELF (%lu bytes)\n", args->elf_size);

    /* Load ELF into the process's address space */
    ElfLoadResult result;
    int err = elf_load(args->elf_data, args->elf_size, p->page_table, &result);
    if (err != 0) {
        kprintf("[INIT] FATAL: elf_load failed (%d)\n", err);
        return;
    }

    /* Set program break */
    p->brk_start = result.brk_start;
    p->brk_current = result.brk_start;

    /* Allocate and map user stack pages */
    if (vmm_map_user_stack(p->page_table) != 0) {
        kprintf("[INIT] FATAL: out of memory for user stack\n");
        return;
    }
    kprintf("[INIT] User stack mapped: 0x%lx - 0x%lx\n",
            USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE), USER_STACK_TOP);

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

    /* Set TSS.rsp0 and SYSCALL kernel RSP */
    Thread *t = thread_current();
    gdt_set_kernel_stack(t->kernel_stack_top);
    syscall_kernel_rsp = t->kernel_stack_top;

    /* Switch to the process's address space */
    paging_write_cr3(p->page_table);

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

    /* Allocate FD table */
    p->fd_table = kmalloc(sizeof(FdTable), 0);
    if (p->fd_table == NULL) {
        kprintf("[INIT] FATAL: cannot allocate fd table\n");
        return -1;
    }
    fd_table_init(p->fd_table);

    /* fds 0, 1, 2 = /dev/tty, inherited by every process (Linux programs expect
     * them open). Without them a process's first open() returned 0-2, so
     * opendir() in the shell took fd 1 and printf output went to a directory. */
    for (int fd = 0; fd < 3; fd++) {
        if (fd_alloc(p->fd_table) != fd ||
            vfs_open("/dev/tty", O_RDWR, fd_get(p->fd_table, fd)) != 0) {
            fd_free(p->fd_table, fd);
            kprintf("[INIT] WARNING: no /dev/tty for fd %d (falls back to the TTY driver)\n", fd);
            break;
        }
    }

    /* Create kernel thread that will load ELF and transition to user mode */
    g_init_args.elf_data = init_mod->address;
    g_init_args.elf_size = init_mod->size;
    g_init_args.path = init_mod->path;
    g_init_args.proc = p;

    Thread *t = thread_create(init_thread_entry, &g_init_args);
    if (t == NULL) {
        kprintf("[INIT] FATAL: thread_create failed\n");
        return -1;
    }

    /* Register thread with process */
    proc_set_main_thread(p, t);
    testmode_set_init_pid((int)p->pid);

    /* Add to scheduler */
    sched_add_thread(t);

    kprintf("[INIT] Init process pid=%u scheduled (tid=%u)\n", p->pid, t->tid);
    return 0;
}
