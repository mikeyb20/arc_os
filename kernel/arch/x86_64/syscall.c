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

    /* Set exit status and mark as zombie for parent to reap */
    p->exit_status = (int32_t)status;
    p->state = PROC_ZOMBIE;

    thread_current()->state = THREAD_DEAD;
    sched_schedule();
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

/* SYS_WRITE: write to fd (fd 1/2 → serial via TTY) */
static int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    /* fd 1 (stdout) and fd 2 (stderr) go to TTY (serial output) */
    if (fd != 1 && fd != 2) {
        return -EBADF;
    }

    if (!user_ptr_valid((const void *)buf_addr, count)) return -EINVAL;
    return tty_write((const void *)buf_addr, (uint32_t)count);
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

    /* fd 0 (stdin) reads from TTY */
    if (fd == 0) return tty_read((void *)buf_addr, (uint32_t)count);

    Process *p = proc_current();
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
    uint64_t old_page = (p->brk_current + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t new_page = (new_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
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
    if (zombie != NULL) {
        int32_t status = 0;
        int pid = proc_reap(zombie, &status);
        if (status_addr != 0) {
            *(int32_t *)status_addr = status;
        }
        return pid;
    }

    /* No zombie yet — check if we have any live children */
    if (!proc_has_children(p)) {
        return -ECHILD;
    }

    /* Children exist but none zombie yet — busy-wait */
    while (proc_find_zombie_child(p) == NULL) {
        sched_yield();
    }

    zombie = proc_find_zombie_child(p);
    int32_t status = 0;
    int pid = proc_reap(zombie, &status);
    if (status_addr != 0) {
        *(int32_t *)status_addr = status;
    }
    return pid;
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
    if (file_size == 0 || file_size > 16 * 1024 * 1024) {
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
                        VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
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
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);

    /* 3. Set LSTAR = syscall entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 4. Set SFMASK: clear IF (bit 9) and DF (bit 10) on SYSCALL entry */
    wrmsr(MSR_SFMASK, (1ULL << 9) | (1ULL << 10));

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

    kprintf("[SYSCALL] Initialized (LSTAR=0x%lx, STAR=0x%lx)\n",
            (uint64_t)syscall_entry, star);
}
