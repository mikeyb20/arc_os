/* arc_os — process, credential and signal syscalls (Linux x86_64 numbers, ADR-0001).
 *
 * Moved out of arch/x86_64/syscall.c by M0-05. */

#include "proc/proc_syscalls.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/fd.h"
#include "proc/elf.h"
#include "proc/signal.h"
#include "proc/waitqueue.h"
#include "proc/spinlock.h"
#include "fs/vfs.h"
#include "fs/pipe.h"
#include "fs/fs_syscalls.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "arch/x86_64/syscall.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/usermode.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/testmode.h"
#include "user_access.h"
#include "uapi/unistd.h"
#include "uapi/wait.h"

/* Maximum ELF binary size for execve */
#define MAX_ELF_SIZE  (16 * 1024 * 1024)  /* 16 MB */

/* execve argv limits */
#define MAX_EXEC_ARGS     32
#define MAX_EXEC_ARG_DATA 4096

/* Linux struct rusage size (wait4 zero-fills it: there is no accounting yet) */
#define RUSAGE_SIZE 144

/* Kernel-side copy of exec argv data */
typedef struct {
    int    argc;
    char  *ptrs[MAX_EXEC_ARGS + 1];  /* Pointers into data[] */
    char   data[MAX_EXEC_ARG_DATA];  /* Packed NUL-terminated strings */
    size_t data_used;
} ExecArgv;

void proc_release_fds(Process *p) {
    if (p->fd_table == NULL) return;
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fd_table->entries[i].in_use) {
            VfsFile *f = &p->fd_table->entries[i].file;
            if (f->node && f->node->type == VFS_PIPE) {
                pipe_close(f->node);
            }
            p->fd_table->entries[i].in_use = 0;
        }
    }
}

/* exit(status) — 60: terminate the current process */
static int64_t sys_exit(uint64_t status, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    kprintf("[SYSCALL] exit(%lu) from pid=%u\n", status & 0xFF, p->pid);

    /* Close all open file descriptors (critical for pipe EOF signaling) */
    proc_release_fds(p);

    /* Only the low 8 bits of the status reach the parent (Linux) */
    p->exit_status = (int32_t)(status & 0xFF);
    p->term_signal = 0;
    p->state = PROC_ZOMBIE;

    /* Kernel test mode: init exiting ends the QEMU run with its status */
    testmode_process_exited((int)p->pid, p->exit_status);

    /* Notify parent of child exit */
    if (p->parent != NULL) {
        wq_wake(&p->parent->child_exit_wq);
        sig_send(p->parent->pid, SIGCHLD);
    }

    thread_current()->state = THREAD_DEAD;
    sched_schedule();
    for (;;) __asm__ volatile ("hlt");
    __builtin_unreachable();  /* suppress -Wreturn-type */
}

/* getpid() — 39 */
static int64_t sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    return p ? (int64_t)p->pid : 0;
}

/* getppid() — 110 */
static int64_t sys_getppid(uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    return (p && p->parent) ? (int64_t)p->parent->pid : 0;
}

/* fork() — 57 */
static int64_t sys_fork(uint64_t a0, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *parent = proc_current();
    if (parent == NULL) return -ENOSYS;

    ForkContext ctx;
    syscall_save_user_context(&ctx);

    Process *child = proc_fork(parent, &ctx);
    if (child == NULL) return -ENOMEM;
    return (int64_t)child->pid;
}

/* Read an ELF binary from VFS into a kmalloc'd buffer.
 * Caller must kfree(*out_buf) on success. */
static int exec_read_elf(const char *path, void **out_buf, uint64_t *out_size) {
    VfsFile file;
    int err = vfs_open(path, O_RDONLY, &file);
    if (err != 0) return err;

    if (file.node->type != VFS_FILE) {
        vfs_close(&file);
        return -EACCES;
    }

    uint64_t file_size = file.node->size;
    if (file_size == 0 || file_size > MAX_ELF_SIZE) {
        vfs_close(&file);
        return -ENOEXEC;
    }

    void *elf_buf = kmalloc(file_size, 0);
    if (elf_buf == NULL) {
        vfs_close(&file);
        return -ENOMEM;
    }

    int bytes = vfs_read(&file, elf_buf, (uint32_t)file_size);
    vfs_close(&file);
    if (bytes < 0 || (uint64_t)bytes != file_size) {
        kfree(elf_buf);
        return -EIO;
    }

    *out_buf = elf_buf;
    *out_size = file_size;
    return 0;
}

/* Copy argv strings from the current (old) user address space into a kernel buffer.
 * Must be called BEFORE destroying the old PML4. tests/test_exec_argv.c keeps a copy. */
static int exec_copy_argv(uint64_t argv_addr, ExecArgv *args) {
    args->argc = 0;
    args->data_used = 0;

    if (argv_addr == 0) return 0;

    if (!user_ptr_valid((const void *)argv_addr, sizeof(uint64_t)))
        return -EFAULT;

    const char *const *user_argv = (const char *const *)argv_addr;

    for (int i = 0; i < MAX_EXEC_ARGS; i++) {
        const char *ustr = user_argv[i];
        if (ustr == (void *)0) break;

        if (!user_ptr_valid(ustr, 1)) return -EFAULT;

        /* Copy string byte-by-byte into data[] */
        args->ptrs[i] = &args->data[args->data_used];
        size_t start = args->data_used;
        while (1) {
            if (args->data_used >= MAX_EXEC_ARG_DATA) return -E2BIG;
            char c = ustr[args->data_used - start];
            args->data[args->data_used++] = c;
            if (c == '\0') break;
        }
        args->argc++;
    }

    /* If we hit MAX_EXEC_ARGS, check if there are more */
    if (args->argc == MAX_EXEC_ARGS) {
        if (user_argv[MAX_EXEC_ARGS] != (void *)0)
            return -E2BIG;
    }

    args->ptrs[args->argc] = (void *)0;
    return 0;
}

/* Write argv data onto the new user stack. Called AFTER paging_write_cr3(new_pml4)
 * so kernel can write directly to user virtual addresses (no SMAP). */
static void exec_setup_user_stack(const ExecArgv *args,
                                  uint64_t *out_rsp, uint64_t *out_argv) {
    uint64_t sp = USER_STACK_TOP;
    uint64_t str_addrs[MAX_EXEC_ARGS];

    /* 1. Copy strings to top of stack (high→low) */
    for (int i = args->argc - 1; i >= 0; i--) {
        size_t len = strlen(args->ptrs[i]) + 1;
        sp -= len;
        memcpy((void *)sp, args->ptrs[i], len);
        str_addrs[i] = sp;
    }

    /* 2. Align to 8 bytes */
    sp &= ~7ULL;

    /* 3. NULL terminator for argv array */
    sp -= 8;
    *(uint64_t *)sp = 0;

    /* 4. argv pointers (reverse order, growing down) */
    for (int i = args->argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = str_addrs[i];
    }
    *out_argv = (args->argc > 0) ? sp : 0;

    /* 5. Align RSP to 16 bytes (ABI) */
    sp &= ~15ULL;
    *out_rsp = sp;
}

/* execve(path, argv, envp) — 59. envp is ignored until M0-06 builds the
 * System V initial stack (argc, argv, envp, auxv). */
static int64_t sys_execve(uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)envp_addr; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;

    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;

    /* 1. Copy argv from OLD address space before destroying it */
    ExecArgv args;
    int argerr = exec_copy_argv(argv_addr, &args);
    if (argerr != 0) return argerr;

    /* 2. Read ELF binary from VFS */
    void *elf_buf;
    uint64_t file_size;
    int err = exec_read_elf(abs, &elf_buf, &file_size);
    if (err != 0) return err;

    /* 3. Create fresh user address space */
    uint64_t old_pml4 = p->page_table;
    uint64_t new_pml4 = vmm_create_user_pml4();
    if (new_pml4 == 0) { kfree(elf_buf); return -ENOMEM; }

    /* 4. Load ELF segments */
    ElfLoadResult result;
    err = elf_load(elf_buf, file_size, new_pml4, &result);
    kfree(elf_buf);
    if (err != 0) { vmm_destroy_user_pml4(new_pml4); return err; }

    /* 5. Map user stack */
    err = vmm_map_user_stack(new_pml4);
    if (err != 0) { vmm_free_user_pages(new_pml4); return err; }

    /* 6. Check setuid/setgid bits on the executable */
    VfsNode *exec_node = vfs_resolve(abs);
    if (exec_node != NULL) {
        if (exec_node->mode & S_ISUID) {
            p->euid = exec_node->uid;
        }
        if (exec_node->mode & S_ISGID) {
            p->egid = exec_node->gid;
        }
    }

    /* 7. Switch to new address space */
    p->page_table = new_pml4;
    p->brk_start = result.brk_start;
    p->brk_current = result.brk_start;
    vmm_free_user_pages(old_pml4);
    paging_write_cr3(new_pml4);

    /* 8. Write argv onto new user stack */
    uint64_t user_rsp, argv_ptr;
    exec_setup_user_stack(&args, &user_rsp, &argv_ptr);

    kprintf("[EXEC] pid=%u loaded '%s' argc=%d entry=0x%lx\n",
            p->pid, abs, args.argc, result.entry_point);
    jump_to_usermode(result.entry_point, user_rsp, (uint64_t)args.argc, argv_ptr);
}

/* Linux wait status words (see uapi/wait.h) */
static int32_t wait_status_exited(const Process *child) {
    if (child->term_signal != 0) return child->term_signal & 0x7F;
    return (child->exit_status & 0xFF) << 8;
}
static int32_t wait_status_stopped(int signo) {
    return ((signo & 0xFF) << 8) | 0x7F;
}

/* Serializes wait4 against itself; children wake the parent's child_exit_wq. */
static Spinlock wait_lock = SPINLOCK_INIT;

/* wait4(pid, wstatus, options, rusage) — 61 */
static int64_t sys_wait4(uint64_t pid_arg, uint64_t status_addr, uint64_t options,
                         uint64_t rusage_addr, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ECHILD;

    int32_t pid = (int32_t)pid_arg;
    if (pid == INT32_MIN) return -ESRCH;   /* -pid would overflow (Linux) */
    if (options & ~(uint64_t)(WNOHANG | WUNTRACED | WCONTINUED)) return -EINVAL;
    if (status_addr != 0 && !user_ptr_valid((void *)status_addr, sizeof(int32_t)))
        return -EFAULT;
    if (rusage_addr != 0 && !user_ptr_valid((void *)rusage_addr, RUSAGE_SIZE))
        return -EFAULT;

    int want_stopped = (options & WUNTRACED) != 0;
    int64_t ret;
    int32_t encoded = 0;

    spinlock_acquire(&wait_lock);
    for (;;) {
        Process *child = proc_find_waitable(p, pid, want_stopped);
        if (child != NULL && child->state == PROC_ZOMBIE) {
            encoded = wait_status_exited(child);
            ret = proc_reap(child, NULL);
            break;
        }
        if (child != NULL) {
            /* Stopped and not yet reported */
            encoded = wait_status_stopped(child->exit_status);
            child->exit_status = 0;   /* Mark as reported */
            ret = (int64_t)child->pid;
            break;
        }
        if (!proc_has_selected_child(p, pid)) {
            spinlock_release(&wait_lock);
            return -ECHILD;
        }
        if (options & WNOHANG) {
            spinlock_release(&wait_lock);
            return 0;   /* Children exist but none is ready */
        }
        wq_sleep(&p->child_exit_wq, &wait_lock);
        spinlock_acquire(&wait_lock);
    }
    spinlock_release(&wait_lock);

    if (status_addr != 0) *(int32_t *)status_addr = encoded;
    if (rusage_addr != 0) memset((void *)rusage_addr, 0, RUSAGE_SIZE);
    return ret;
}

/* kill(pid, sig) — 62: pid > 0 one process, 0 own group, < 0 group -pid */
static int64_t sys_kill(uint64_t pid_arg, uint64_t signo, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    int32_t pid = (int32_t)pid_arg;
    if (pid < 0) {
        if (pid == -1 || pid == INT32_MIN) return -ESRCH;   /* broadcast: not supported */
        return sig_send_group((pid_t)(-pid), (int)signo);
    }
    if (pid == 0) {
        Process *p = proc_current();
        if (p == NULL) return -ESRCH;
        return sig_send_group(p->pgid, (int)signo);
    }
    return sig_send((uint32_t)pid, (int)signo);
}

/* rt_sigaction(sig, act, oact, sigsetsize) — 13. Stores the handler only:
 * sa_flags, sa_mask and sa_restorer are accepted and ignored, and delivery still
 * uses arc_os's own frame and trampoline. M1-01 implements them. */
static int64_t sys_rt_sigaction(uint64_t signo, uint64_t act_addr, uint64_t oact_addr,
                                uint64_t sigsetsize, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ESRCH;
    if (sigsetsize != ARC_SIGSET_SIZE) return -EINVAL;
    if (signo < 1 || signo >= NSIG) return -EINVAL;
    if (act_addr != 0 && (signo == SIGKILL || signo == SIGSTOP)) return -EINVAL;
    if (act_addr != 0 && !user_ptr_valid((const void *)act_addr, sizeof(struct kernel_sigaction)))
        return -EFAULT;
    if (oact_addr != 0 && !user_ptr_valid((void *)oact_addr, sizeof(struct kernel_sigaction)))
        return -EFAULT;

    sig_handler_t old = p->sig.handlers[signo];
    if (act_addr != 0) {
        /* Read act before writing oact: they may be the same buffer */
        struct kernel_sigaction act = *(const struct kernel_sigaction *)act_addr;
        sig_set_handler(&p->sig, (int)signo, (sig_handler_t)act.sa_handler);
    }
    if (oact_addr != 0) {
        struct kernel_sigaction *oact = (struct kernel_sigaction *)oact_addr;
        memset(oact, 0, sizeof(*oact));
        oact->sa_handler = (uint64_t)old;
    }
    return 0;
}

/* rt_sigreturn() — 15: restore the context saved by signal delivery */
static int64_t sys_rt_sigreturn(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;

    /* The handler's ret popped ret_addr, the frame's last field, so the frame
     * ends at the trampoline's RSP (see sig_setup_frame). */
    uint64_t frame_addr = syscall_user_rsp() - sizeof(SignalFrame);
    if (!user_ptr_valid((void *)frame_addr, sizeof(SignalFrame))) return -EFAULT;

    SignalFrame *sf = (SignalFrame *)frame_addr;
    p->sig.restore_frame = *sf;
    p->sig.restoring = 1;
    return 0;  /* dummy — sig_maybe_deliver will override RAX */
}

/* getuid() — 102: real user ID */
static int64_t sys_getuid(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    return p ? (int64_t)p->uid : 0;
}

/* getgid() — 104: real group ID */
static int64_t sys_getgid(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    return p ? (int64_t)p->gid : 0;
}

/* setuid(uid) — 105: root only (sets real and effective) */
static int64_t sys_setuid(uint64_t new_uid, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->euid != 0) return -EPERM;
    p->uid = (uint32_t)new_uid;
    p->euid = (uint32_t)new_uid;
    return 0;
}

/* setgid(gid) — 106: root only (sets real and effective) */
static int64_t sys_setgid(uint64_t new_gid, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->euid != 0) return -EPERM;
    p->gid = (uint32_t)new_gid;
    p->egid = (uint32_t)new_gid;
    return 0;
}

/* setpgid(pid, pgid) — 109: 0 means the caller / the target's own pid */
static int64_t sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    Process *cur = proc_current();
    if (cur == NULL) return -ESRCH;
    if ((int32_t)pid_arg < 0 || (int32_t)pgid_arg < 0) return -EINVAL;

    Process *target = (pid_arg == 0) ? cur : proc_get_by_pid((uint32_t)pid_arg);
    if (target == NULL) return -ESRCH;

    target->pgid = (pgid_arg == 0) ? target->pid : (pid_t)pgid_arg;
    return 0;
}

/* getpgid(pid) — 121 */
static int64_t sys_getpgid(uint64_t pid_arg, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *cur = proc_current();
    if (cur == NULL) return -ESRCH;
    if (pid_arg == 0) return (int64_t)cur->pgid;

    Process *target = proc_get_by_pid((uint32_t)pid_arg);
    if (target == NULL) return -ESRCH;
    return (int64_t)target->pgid;
}

void proc_syscalls_init(void) {
    syscall_register(__NR_rt_sigaction, sys_rt_sigaction);
    syscall_register(__NR_rt_sigreturn, sys_rt_sigreturn);
    syscall_register(__NR_getpid,       sys_getpid);
    syscall_register(__NR_fork,         sys_fork);
    syscall_register(__NR_execve,       sys_execve);
    syscall_register(__NR_exit,         sys_exit);
    syscall_register(__NR_wait4,        sys_wait4);
    syscall_register(__NR_kill,         sys_kill);
    syscall_register(__NR_getuid,       sys_getuid);
    syscall_register(__NR_getgid,       sys_getgid);
    syscall_register(__NR_setuid,       sys_setuid);
    syscall_register(__NR_setgid,       sys_setgid);
    syscall_register(__NR_setpgid,      sys_setpgid);
    syscall_register(__NR_getppid,      sys_getppid);
    syscall_register(__NR_getpgid,      sys_getpgid);
}
