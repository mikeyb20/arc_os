#include "arch/x86_64/syscall.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/serial.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/fd.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "proc/elf.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/usermode.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "fs/vfs.h"
#include "user_access.h"
#include "drivers/tty.h"
#include "fs/pipe.h"
#include "proc/signal.h"

/* Exported from syscall_entry.asm: user context saved during SYSCALL */
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;
extern uint64_t syscall_saved_user_rbp;
extern uint64_t syscall_saved_user_rbx;
extern uint64_t syscall_saved_user_r12;
extern uint64_t syscall_saved_user_r13;
extern uint64_t syscall_saved_user_r14;
extern uint64_t syscall_saved_user_r15;

/* RFLAGS bits cleared by SFMASK on SYSCALL entry */
#define RFLAGS_IF  (1ULL << 9)   /* Interrupt Flag */
#define RFLAGS_DF  (1ULL << 10)  /* Direction Flag */

/* Standard file descriptor numbers */
#define FD_STDOUT  1
#define FD_STDERR  2

/* Maximum ELF binary size for sys_exec */
#define MAX_ELF_SIZE  (16 * 1024 * 1024)  /* 16 MB */

/* Syscall handler table */
static syscall_handler_t syscall_table[SYSCALL_MAX];

/* Kernel RSP for SYSCALL entry — set by scheduler on context switch */
uint64_t syscall_kernel_rsp;

/* --- Built-in syscall handlers --- */

/* SYS_EXIT: terminate current process */
static int64_t sys_exit(uint64_t status, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    kprintf("[SYSCALL] exit(%lu) from pid=%u\n", status, p->pid);

    /* Close all open file descriptors (critical for pipe EOF signaling) */
    if (p->fd_table != NULL) {
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

    /* Notify parent of child exit */
    if (p->parent != NULL) {
        sig_send(p->parent->pid, SIGCHLD);
    }

    /* Set exit status and mark as zombie for parent to reap */
    p->exit_status = (int32_t)status;
    p->state = PROC_ZOMBIE;

    thread_current()->state = THREAD_DEAD;
    sched_schedule();
    for (;;) __asm__ volatile ("hlt");
    __builtin_unreachable();  /* suppress -Wreturn-type */
}

/* SYS_WRITE: write to fd (fd 1/2 → TTY unless redirected via dup2) */
static int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (!user_ptr_valid((const void *)buf_addr, count)) return -EINVAL;

    Process *p = proc_current();

    if (fd == 1 || fd == 2) {
        /* Check fd table first — dup2 may have redirected stdout/stderr */
        if (p != NULL && p->fd_table != NULL) {
            VfsFile *file = fd_get(p->fd_table, (int)fd);
            if (file != NULL) {
                int64_t ret = vfs_write(file, (const void *)buf_addr, (uint32_t)count);
                if (ret == -EPIPE) sig_send(p->pid, SIGPIPE);
                return ret;
            }
        }
        /* Default: TTY output */
        return tty_write((const void *)buf_addr, (uint32_t)count);
    }

    /* Other fds — look up in fd table */
    if (p == NULL || p->fd_table == NULL) return -EBADF;
    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;
    int64_t ret = vfs_write(file, (const void *)buf_addr, (uint32_t)count);
    if (ret == -EPIPE && p != NULL) {
        sig_send(p->pid, SIGPIPE);
    }
    return ret;
}

/* SYS_GETPID: return current process ID */
static int64_t sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    return p ? (int64_t)p->pid : -1;
}

/* SYS_OPEN: open a file by path */
static int64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)mode; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -ENOSYS;

    int fd = fd_alloc(p->fd_table);
    if (fd < 0) return -ENOMEM;

    if (!user_ptr_valid((const void *)path_addr, 1)) return -EINVAL;
    const char *path = (const char *)path_addr;
    VfsFile *file = fd_get(p->fd_table, fd);
    int err = vfs_open(path, (uint32_t)flags, file);
    if (err != 0) {
        fd_free(p->fd_table, fd);
        return err;
    }
    return fd;
}

/* SYS_READ: read from a file descriptor */
static int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (!user_ptr_valid((void *)buf_addr, count)) return -EINVAL;

    Process *p = proc_current();

    if (fd == 0) {
        /* Check fd table first — dup2 may have redirected stdin */
        if (p != NULL && p->fd_table != NULL) {
            VfsFile *file = fd_get(p->fd_table, 0);
            if (file != NULL) {
                return vfs_read(file, (void *)buf_addr, (uint32_t)count);
            }
        }
        /* Default: TTY input */
        return tty_read((void *)buf_addr, (uint32_t)count);
    }

    if (p == NULL || p->fd_table == NULL) return -ENOSYS;

    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;

    return vfs_read(file, (void *)buf_addr, (uint32_t)count);
}

/* SYS_CLOSE: close a file descriptor */
static int64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -ENOSYS;

    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;

    if (file->node && file->node->type == VFS_PIPE) {
        pipe_close(file->node);
    }
    vfs_close(file);
    fd_free(p->fd_table, (int)fd);
    return 0;
}

/* SYS_BRK: adjust program break */
static int64_t sys_brk(uint64_t new_brk, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;

    /* If new_brk is 0, return current break */
    if (new_brk == 0) return (int64_t)p->brk_current;

    /* Only allow growing, not shrinking below start */
    if (new_brk < p->brk_start) return (int64_t)p->brk_current;

    /* Map new pages if growing */
    uint64_t old_page = PAGE_ALIGN_UP(p->brk_current);
    uint64_t new_page = PAGE_ALIGN_UP(new_brk);
    uint64_t hhdm = vmm_get_hhdm_offset();

    for (uint64_t vaddr = old_page; vaddr < new_page; vaddr += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) return (int64_t)p->brk_current;
        memset((void *)(phys + hhdm), 0, PAGE_SIZE);
        vmm_map_page_in(p->page_table, vaddr, phys,
                        VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
    }

    p->brk_current = new_brk;
    return (int64_t)new_brk;
}

/* SYS_LSEEK: reposition file offset */
static int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -ENOSYS;

    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;

    if (file->node && file->node->type == VFS_PIPE) return -ESPIPE;

    return vfs_seek(file, (int64_t)offset, (int)whence);
}

/* SYS_STAT: get file metadata by path */
static int64_t sys_stat(uint64_t path_addr, uint64_t stat_addr, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (!user_ptr_valid((const void *)path_addr, 1)) return -EINVAL;
    if (!user_ptr_valid((void *)stat_addr, sizeof(VfsStat))) return -EINVAL;
    const char *path = (const char *)path_addr;
    VfsStat *out = (VfsStat *)stat_addr;

    return vfs_stat(path, out);
}

/* SYS_MKDIR: create a directory */
static int64_t sys_mkdir(uint64_t path_addr, uint64_t mode, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (!user_ptr_valid((const void *)path_addr, 1)) return -EINVAL;
    const char *path = (const char *)path_addr;

    return vfs_mkdir(path, (uint32_t)mode);
}

/* SYS_READDIR: list directory entries */
static int64_t sys_readdir(uint64_t path_addr, uint64_t entries_addr, uint64_t max,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (!user_ptr_valid((const void *)path_addr, 1)) return -EINVAL;
    if (max > 0 && !user_ptr_valid((void *)entries_addr, max * sizeof(VfsDirEntry))) return -EINVAL;
    const char *path = (const char *)path_addr;
    VfsDirEntry *entries = (VfsDirEntry *)entries_addr;

    return vfs_readdir(path, entries, (uint32_t)max);
}

/* SYS_UNLINK: delete a file */
static int64_t sys_unlink(uint64_t path_addr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (!user_ptr_valid((const void *)path_addr, 1)) return -EINVAL;
    const char *path = (const char *)path_addr;

    return vfs_unlink(path);
}

/* Reap a zombie child and optionally copy exit status to user space */
static int reap_zombie(Process *zombie, uint64_t status_addr) {
    int32_t status = 0;
    int pid = proc_reap(zombie, &status);
    if (status_addr != 0) {
        *(int32_t *)status_addr = status;
    }
    return pid;
}

/* SYS_WAIT: wait for a child to exit, return child PID */
static int64_t sys_wait(uint64_t status_addr, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;

    if (status_addr != 0 && !user_ptr_valid((void *)status_addr, sizeof(int32_t)))
        return -EINVAL;

    /* Check for zombie children */
    Process *zombie = proc_find_zombie_child(p);
    if (zombie != NULL) return reap_zombie(zombie, status_addr);

    /* No zombie yet — check if we have any live children */
    if (!proc_has_children(p)) {
        return -ECHILD;
    }

    /* Children exist but none zombie yet — busy-wait */
    while ((zombie = proc_find_zombie_child(p)) == NULL) {
        sched_yield();
    }
    return reap_zombie(zombie, status_addr);
}

/* SYS_FORK: create child process */
static int64_t sys_fork(uint64_t a0, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *parent = proc_current();
    if (parent == NULL) return -ENOSYS;

    ForkContext ctx = {
        .user_rip    = syscall_saved_user_rip,
        .user_rsp    = syscall_saved_user_rsp,
        .user_rflags = syscall_saved_user_rflags,
        .user_rbp    = syscall_saved_user_rbp,
        .user_rbx    = syscall_saved_user_rbx,
        .user_r12    = syscall_saved_user_r12,
        .user_r13    = syscall_saved_user_r13,
        .user_r14    = syscall_saved_user_r14,
        .user_r15    = syscall_saved_user_r15,
    };

    Process *child = proc_fork(parent, &ctx);
    if (child == NULL) return -ENOMEM;

    return (int64_t)child->pid;
}

/* SYS_EXEC: replace process image with new ELF binary */
static int64_t sys_exec(uint64_t path_addr, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;

    if (!user_ptr_valid((const void *)path_addr, 1)) return -EINVAL;
    const char *path = (const char *)path_addr;

    /* 1. Open and read ELF file from VFS */
    VfsFile file;
    int err = vfs_open(path, O_RDONLY, &file);
    if (err != 0) return err;

    uint64_t file_size = file.node->size;
    if (file_size == 0 || file_size > MAX_ELF_SIZE) {
        vfs_close(&file);
        return -EINVAL;
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

    /* 2. Tear down old user address space */
    uint64_t old_pml4 = p->page_table;

    /* 3. Create fresh user address space */
    uint64_t new_pml4 = vmm_create_user_pml4();
    if (new_pml4 == 0) {
        kfree(elf_buf);
        return -ENOMEM;
    }

    /* 4. Load ELF into new address space */
    ElfLoadResult result;
    err = elf_load(elf_buf, file_size, new_pml4, &result);
    kfree(elf_buf);
    if (err != 0) {
        vmm_destroy_user_pml4(new_pml4);
        return err;
    }

    /* 5. Allocate user stack in new address space */
    uint64_t hhdm = vmm_get_hhdm_offset();
    uint64_t stack_bottom = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    for (uint64_t vaddr = stack_bottom; vaddr < USER_STACK_TOP; vaddr += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            vmm_free_user_pages(new_pml4);
            return -ENOMEM;
        }
        memset((void *)(phys + hhdm), 0, PAGE_SIZE);
        vmm_map_page_in(new_pml4, vaddr, phys,
                        VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    }

    /* 6. Update process */
    p->page_table = new_pml4;
    p->brk_start = result.brk_start;
    p->brk_current = result.brk_start;

    /* 7. Free old address space */
    vmm_free_user_pages(old_pml4);

    /* 8. Switch to new page tables and jump to new entry */
    paging_write_cr3(new_pml4);

    kprintf("[EXEC] pid=%u loaded '%s' entry=0x%lx\n", p->pid, path, result.entry_point);

    /* Does not return — jumps to user mode */
    jump_to_usermode(result.entry_point, USER_STACK_TOP);
}

/* SYS_DUP2: duplicate a file descriptor */
static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -ENOSYS;
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return (int64_t)newfd;

    VfsFile *src = fd_get(p->fd_table, (int)oldfd);
    if (src == NULL) return -EBADF;

    /* If newfd is open, close it first */
    FdEntry *dst_entry = &p->fd_table->entries[newfd];
    if (dst_entry->in_use) {
        if (dst_entry->file.node && dst_entry->file.node->type == VFS_PIPE) {
            pipe_close(dst_entry->file.node);
        }
        dst_entry->in_use = 0;
    }

    /* Copy the file entry */
    dst_entry->file = *src;
    dst_entry->in_use = 1;

    /* If it's a pipe, bump the ref count */
    if (dst_entry->file.node && dst_entry->file.node->type == VFS_PIPE) {
        pipe_addref(dst_entry->file.node);
    }

    return (int64_t)newfd;
}

/* Initialize a pipe fd entry */
static void pipe_setup_fd(FdTable *ft, int fd, VfsNode *node, uint32_t flags) {
    VfsFile *f = fd_get(ft, fd);
    f->node = node;
    f->offset = 0;
    f->flags = flags;
}

/* SYS_PIPE: create a pipe */
static int64_t sys_pipe(uint64_t pipefd_addr, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (!user_ptr_valid((void *)pipefd_addr, 2 * sizeof(int32_t))) return -EINVAL;

    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -ENOSYS;

    VfsNode *read_node, *write_node;
    int err = pipe_create(&read_node, &write_node);
    if (err != 0) return err;

    int rfd = fd_alloc(p->fd_table);
    if (rfd < 0) {
        pipe_close(read_node);
        pipe_close(write_node);
        return -ENOMEM;
    }

    int wfd = fd_alloc(p->fd_table);
    if (wfd < 0) {
        fd_free(p->fd_table, rfd);
        pipe_close(read_node);
        pipe_close(write_node);
        return -ENOMEM;
    }

    pipe_setup_fd(p->fd_table, rfd, read_node, O_RDONLY);
    pipe_setup_fd(p->fd_table, wfd, write_node, O_WRONLY);

    /* Write fds to user space */
    int32_t *user_fds = (int32_t *)pipefd_addr;
    user_fds[0] = (int32_t)rfd;
    user_fds[1] = (int32_t)wfd;

    return 0;
}

/* SYS_SIGNAL: register a signal handler */
static int64_t sys_signal(uint64_t signo, uint64_t handler_addr, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;
    sig_handler_t old = sig_set_handler(&p->sig, (int)signo, (sig_handler_t)handler_addr);
    return (int64_t)(uint64_t)old;
}

/* SYS_KILL: send signal to a process */
static int64_t sys_kill(uint64_t pid, uint64_t signo, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return sig_send((uint32_t)pid, (int)signo);
}

/* SYS_SIGRETURN: restore context after signal handler */
static int64_t sys_sigreturn(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOSYS;

    /* Read SignalFrame from user RSP */
    uint64_t user_rsp = syscall_saved_user_rsp;
    if (!user_ptr_valid((void *)user_rsp, sizeof(SignalFrame))) return -EINVAL;

    SignalFrame *sf = (SignalFrame *)user_rsp;
    p->sig.restore_frame = *sf;
    p->sig.restoring = 1;
    return 0;  /* dummy — sig_maybe_deliver will override RAX */
}

/* --- Dispatcher --- */

int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (num >= SYSCALL_MAX || syscall_table[num] == NULL) {
        return -ENOSYS;
    }
    return syscall_table[num](a0, a1, a2, a3, a4, a5);
}

/* --- Registration --- */

void syscall_register(uint32_t num, syscall_handler_t handler) {
    if (num < SYSCALL_MAX) {
        syscall_table[num] = handler;
    }
}

/* --- Initialization --- */

void syscall_init(void) {
    /* 1. Enable SYSCALL/SYSRET in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    /* 2. Set STAR MSR: kernel CS/SS and user CS/SS base
     *    STAR[47:32] = kernel CS (0x08); kernel SS = CS + 8 = 0x10
     *    STAR[63:48] = user base (0x10); SYSRET loads CS = base+16 = 0x20|3, SS = base+8 = 0x18|3
     *    This matches our GDT layout: 0x08=kcode, 0x10=kdata, 0x18=udata, 0x20=ucode */
    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);

    /* 3. Set LSTAR = syscall entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 4. Set SFMASK: clear IF (bit 9) and DF (bit 10) on SYSCALL entry */
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF);

    /* 5. Register built-in handlers */
    syscall_register(SYS_EXIT,   sys_exit);
    syscall_register(SYS_WRITE,  sys_write);
    syscall_register(SYS_GETPID, sys_getpid);
    syscall_register(SYS_OPEN,   sys_open);
    syscall_register(SYS_READ,   sys_read);
    syscall_register(SYS_CLOSE,  sys_close);
    syscall_register(SYS_BRK,    sys_brk);
    syscall_register(SYS_LSEEK,   sys_lseek);
    syscall_register(SYS_STAT,    sys_stat);
    syscall_register(SYS_MKDIR,   sys_mkdir);
    syscall_register(SYS_READDIR, sys_readdir);
    syscall_register(SYS_UNLINK,  sys_unlink);
    syscall_register(SYS_FORK,    sys_fork);
    syscall_register(SYS_EXEC,    sys_exec);
    syscall_register(SYS_WAIT,    sys_wait);
    syscall_register(SYS_DUP2,      sys_dup2);
    syscall_register(SYS_PIPE,      sys_pipe);
    syscall_register(SYS_SIGNAL,    sys_signal);
    syscall_register(SYS_KILL,      sys_kill);
    syscall_register(SYS_SIGRETURN, sys_sigreturn);

    kprintf("[SYSCALL] Initialized (LSTAR=0x%lx, STAR=0x%lx)\n",
            (uint64_t)syscall_entry, star);
}
