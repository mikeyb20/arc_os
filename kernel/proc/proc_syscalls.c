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
#include "proc/kernel_lock.h"
#include "fs/vfs.h"
#include "fs/fs_syscalls.h"
#include "mm/vmm.h"
#include "mm/mm.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/testmode.h"
#include "user_access.h"
#include "uapi/unistd.h"
#include "uapi/wait.h"
#include "uapi/prctl.h"
#include "proc/exec_stack.h"
#include "fs/path.h"
#include "arch/hal.h"

/* Maximum ELF binary size for execve */
#define MAX_ELF_SIZE  (16 * 1024 * 1024)  /* 16 MB */

/* Linux struct rusage size (wait4 zero-fills it: there is no accounting yet) */
#define RUSAGE_SIZE 144

void proc_release_fds(Process *p) {
    if (p->fd_table == NULL) return;
    for (int i = 0; i < MAX_FDS; i++) {
        fd_close(p->fd_table, i);
    }
}

void proc_exit(Process *p, int32_t exit_status, uint8_t term_signal) {
    /* Close all open file descriptors (critical for pipe EOF signaling) */
    proc_release_fds(p);

    /* The address space goes now, not at reap: a zombie holds no memory.
     * Off this CPU first (no CPU may have it loaded when it is freed). */
    Mm *mm = p->mm;
    p->mm = NULL;
    mm_switch(NULL);
    mm_put(mm);

    p->exit_status = exit_status;
    p->term_signal = term_signal;
    Process *heir = proc_reparent_children(p);
    p->state = PROC_ZOMBIE;
    /* DEAD before any wake-up: the scheduler never queues it again, and
     * sched_wake ignores it. The parent may reap as soon as this thread gives
     * up the kernel lock in sched_schedule(); proc_reap then waits until the
     * switch away has finished before it frees the stack (sched_wait_off_cpu). */
    thread_current()->state = THREAD_DEAD;

    /* Kernel test mode: init exiting ends the QEMU run with its status */
    testmode_process_exited((int)p->pid, p->exit_status);

    if (heir != NULL) {               /* init adopted a zombie: let it reap */
        wq_wake(&heir->child_exit_wq);
        sig_send(heir->pid, SIGCHLD);
    }
    if (p->parent != NULL) {
        wq_wake(&p->parent->child_exit_wq);
        sig_send(p->parent->pid, SIGCHLD);
    }

    sched_schedule();
    for (;;) __asm__ volatile ("hlt");
}

/* exit(status) — 60: terminate the current process */
static int64_t sys_exit(uint64_t status, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    Process *p = proc_current();
    kdebug("[SYSCALL] exit(%lu) from pid=%u\n", status & 0xFF, p->pid);
    /* Only the low 8 bits of the status reach the parent (Linux) */
    proc_exit(p, (int32_t)(status & 0xFF), 0);
}

/* exit_group(status) — 231. A process holds one thread until M1-16, so this
 * is exit(); M1-16 makes it end every thread of the group. */
static int64_t sys_exit_group(uint64_t status, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    return sys_exit(status, a1, a2, a3, a4, a5);
}

/* arch_prctl(code, addr) — 158. Only the FS base (TLS, ADR-0002). */
static int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= USER_ADDR_LIMIT) return -EPERM;   /* Linux: past TASK_SIZE */
        arch_set_fs_base(addr);
        return 0;
    case ARCH_GET_FS: {
        uint64_t base = thread_current()->fs_base;
        return copy_to_user((void *)addr, &base, sizeof(base));
    }
    default:
        return -EINVAL;   /* ARCH_SET_GS/GET_GS and the rest: not supported */
    }
}

/* set_tid_address(tidptr) — 218. Stores the pointer that M1-16's thread exit
 * clears (and futex-wakes); returns the caller's tid, which equals the pid
 * while a process holds one thread. */
static int64_t sys_set_tid_address(uint64_t tidptr, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    thread_current()->clear_child_tid = tidptr;
    return p ? (int64_t)p->pid : 0;
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

/* Copy one NULL-terminated user vector (argv or envp) into s, string by
 * string, with the fault-free helpers. Returns 0, -EFAULT or -E2BIG. */
static int exec_copy_vector(uint64_t uvec, ExecStrings *s, int is_env) {
    if (uvec == 0) return 0;   /* Linux accepts NULL as an empty vector */
    for (uint64_t i = 0;; i++) {
        uint64_t uptr;
        if (copy_from_user(&uptr, (const void *)(uvec + 8 * i), sizeof(uptr)) != 0)
            return -EFAULT;
        if (uptr == 0) return 0;

        size_t room = exec_strings_room(s);
        if (room == 0) return -E2BIG;
        long len = strncpy_from_user(s->data + s->used, (const char *)uptr, room);
        if (len == -ENAMETOOLONG) return -E2BIG;
        if (len < 0) return (int)len;
        exec_strings_commit(s, (size_t)len + 1, is_env);
    }
}

void proc_enter_user(Process *p, const ElfLoadResult *elf, ExecStrings *strings,
                     const char *execfn, uint8_t *stack_buf) {
    ExecAux aux;
    aux.phdr  = elf->phdr_addr;
    aux.phent = elf->phent;
    aux.phnum = elf->phnum;
    aux.entry = elf->entry_point;
    aux.uid   = p->uid;
    aux.euid  = p->euid;
    aux.gid   = p->gid;
    aux.egid  = p->egid;
    /* Linux sets AT_SECURE for setuid programs, and musl then poll()s fds 0-2
     * and crashes if poll fails. Always 0 until poll exists (M1-18). */
    aux.secure = 0;
    aux.hwcap = cpu_hwcap();
    aux.hwcap2 = cpu_hwcap2();
    aux.minsigstksz = sig_frame_max_size();
    cpu_random_fill(aux.random, sizeof(aux.random));

    /* Fits by construction: EXEC_STACK_BUF covers EXEC_ARG_MAX plus the rest */
    uint64_t base = USER_STACK_TOP - EXEC_STACK_BUF;
    uint64_t rsp = exec_build_stack(stack_buf, EXEC_STACK_BUF, USER_STACK_TOP,
                                    strings, execfn, &aux);
    if (rsp == 0) {
        kprintf("[EXEC] FATAL: initial stack does not fit\n");
        KERNEL_PANIC();
    }

    /* p's address space is live (CR3): copy the stack image in (its pages
     * come on demand; a larger image grows the stack VMA) */
    if (copy_to_user((void *)rsp, stack_buf + (rsp - base), USER_STACK_TOP - rsp) != 0) {
        kprintf("[EXEC] pid %u: no memory for the initial stack\n", p->pid);
        kfree(stack_buf);
        kfree(strings);
        sig_fatal(SIGKILL);
    }
    kfree(stack_buf);
    kfree(strings);

    /* Fresh user CPU state: no TLS yet, clean FPU/SSE, no clear_child_tid */
    arch_set_fs_base(0);
    fpu_load_init();
    thread_current()->clear_child_tid = 0;

    kernel_unlock();   /* taken at syscall entry (execve) or by init_thread_entry */
    jump_to_usermode(elf->entry_point, rsp);
}

/* execve(path, argv, envp) — 59. Builds the System V initial stack (argc,
 * argv, envp, auxv; ADR-0002) for a static ELF (ET_EXEC or static-PIE). */
static int64_t sys_execve(uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;

    /* The path as given (AT_EXECFN) and resolved against the cwd */
    char raw[PATH_MAX];
    long rlen = strncpy_from_user(raw, (const char *)path_addr, sizeof(raw));
    if (rlen < 0) return rlen;
    if (rlen == 0) return -ENOENT;
    char abs[PATH_MAX];
    int perr = path_normalize(p->cwd, raw, abs, sizeof(abs));
    if (perr != 0) return perr;

    /* 1. Copy argv and envp from the OLD address space before destroying it */
    ExecStrings *strings = kmalloc(sizeof(ExecStrings), 0);
    uint8_t *stack_buf = kmalloc(EXEC_STACK_BUF, 0);
    if (strings == NULL || stack_buf == NULL) {
        kfree(strings);
        kfree(stack_buf);
        return -ENOMEM;
    }
    exec_strings_init(strings);
    int err = exec_copy_vector(argv_addr, strings, 0);
    if (err == 0 && strings->argc == 0) {
        err = exec_strings_add(strings, "", 0);   /* Linux: argv[0] = "" */
    }
    if (err == 0) err = exec_copy_vector(envp_addr, strings, 1);

    /* 2. Read ELF binary from VFS */
    void *elf_buf = NULL;
    uint64_t file_size = 0;
    if (err == 0) err = exec_read_elf(abs, &elf_buf, &file_size);

    /* 3. Fresh user address space with the ELF segments and the stack */
    Mm *new_mm = NULL;
    ElfLoadResult result;
    if (err == 0) {
        new_mm = mm_create();
        if (new_mm == NULL) err = -ENOMEM;
    }
    if (err == 0) err = elf_load(elf_buf, file_size, new_mm, &result);
    kfree(elf_buf);
    if (err == 0) err = mm_setup_stack(new_mm, result.stack_exec, result.brk_start);
    if (err != 0) {
        mm_put(new_mm);
        kfree(strings);
        kfree(stack_buf);
        return err;
    }

    /* 4. setuid/setgid bits on the executable */
    VfsNode *exec_node = vfs_resolve(abs);
    if (exec_node != NULL) {
        if (exec_node->mode & S_ISUID) {
            p->euid = exec_node->uid;
        }
        if (exec_node->mode & S_ISGID) {
            p->egid = exec_node->gid;
        }
    }

    /* 5. Point of no return: switch to the new address space */
    Mm *old_mm = p->mm;
    p->mm = new_mm;
    sig_exec_reset(&p->sig);
    mm_switch(new_mm);
    mm_put(old_mm);

    kdebug("[EXEC] pid=%u loaded '%s' argc=%u envc=%u entry=0x%lx\n", p->pid, abs, strings->argc, strings->envc,
           result.entry_point);

    /* 6. Initial stack, clean CPU state, enter the program */
    proc_enter_user(p, &result, strings, raw, stack_buf);
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
static Spinlock wait_lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "wait4");

/* wait4(pid, wstatus, options, rusage) — 61 */
static int64_t sys_wait4(uint64_t pid_arg, uint64_t status_addr, uint64_t options,
                         uint64_t rusage_addr, uint64_t a4, uint64_t a5) {
    uint64_t irqf;
    (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ECHILD;

    int32_t pid = (int32_t)pid_arg;
    if (pid == INT32_MIN) return -ESRCH;   /* -pid would overflow (Linux) */
    if (options & ~(uint64_t)(WNOHANG | WUNTRACED | WCONTINUED)) return -EINVAL;
    if (status_addr != 0 && !user_ptr_valid((void *)status_addr, sizeof(int32_t)))
        return -EFAULT;
    if (rusage_addr != 0 && !user_ptr_valid((void *)rusage_addr, RUSAGE_SIZE))
        return -EFAULT;   /* early, as Linux: before a child is reaped */

    int want_stopped = (options & WUNTRACED) != 0;
    int64_t ret;
    int32_t encoded = 0;

    spin_lock_irqsave(&wait_lock, &irqf);
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
            spin_unlock_irqrestore(&wait_lock, irqf);
            return -ECHILD;
        }
        if (options & WNOHANG) {
            spin_unlock_irqrestore(&wait_lock, irqf);
            return 0;   /* Children exist but none is ready */
        }
        wq_sleep(&p->child_exit_wq, &wait_lock, irqf);
        spin_lock_irqsave(&wait_lock, &irqf);
    }
    spin_unlock_irqrestore(&wait_lock, irqf);

    /* The child is reaped already: a fault now still reports it (Linux) */
    if (status_addr != 0 && put_user(encoded, (int32_t *)status_addr) != 0) return -EFAULT;
    if (rusage_addr != 0) {
        static const uint8_t zero[RUSAGE_SIZE];
        if (copy_to_user((void *)rusage_addr, zero, RUSAGE_SIZE) != 0) return -EFAULT;
    }
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

/* rt_sigaction(sig, act, oact, sigsetsize) — 13. Linux semantics: handler,
 * sa_flags (SA_RESTORER, SA_NODEFER, SA_RESETHAND act at delivery; the rest
 * are stored), sa_restorer and sa_mask. */
static int64_t sys_rt_sigaction(uint64_t signo, uint64_t act_addr, uint64_t oact_addr,
                                uint64_t sigsetsize, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ESRCH;
    if (sigsetsize != ARC_SIGSET_SIZE) return -EINVAL;
    if (signo < 1 || signo >= NSIG) return -EINVAL;
    if (act_addr != 0 && (signo == SIGKILL || signo == SIGSTOP)) return -EINVAL;

    struct kernel_sigaction act, old;
    if (act_addr != 0 && copy_from_user(&act, (const void *)act_addr, sizeof(act)) != 0) return -EFAULT;
    if (oact_addr != 0 && !user_ptr_valid((void *)oact_addr, sizeof(old))) return -EFAULT;
    int err = sig_set_action(&p->sig, (int)signo, act_addr ? &act : NULL, &old);
    if (err != 0) return err;
    if (oact_addr != 0 && copy_to_user((void *)oact_addr, &old, sizeof(old)) != 0) return -EFAULT;
    return 0;
}

/* rt_sigreturn() — 15. The handler's ret popped pretcode, so the frame starts
 * 8 bytes below the restorer's RSP; the context comes back at syscall exit. */
static int64_t sys_rt_sigreturn(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;
    sig_request_return(&p->sig, syscall_user_rsp() - sizeof(uint64_t));
    return 0;  /* replaced by the frame's RAX in sig_maybe_deliver */
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
    syscall_register(__NR_exit_group,   sys_exit_group);
    syscall_register(__NR_arch_prctl,   sys_arch_prctl);
    syscall_register(__NR_set_tid_address, sys_set_tid_address);
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
