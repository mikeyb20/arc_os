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
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "fs/vfs.h"

/* RFLAGS bits cleared by SFMASK on SYSCALL entry */
#define RFLAGS_IF  (1ULL << 9)   /* Interrupt Flag */
#define RFLAGS_DF  (1ULL << 10)  /* Direction Flag */

/* Standard file descriptor numbers */
#define FD_STDOUT  1
#define FD_STDERR  2

/* Syscall handler table */
static syscall_handler_t syscall_table[SYSCALL_MAX];

/* Kernel RSP for SYSCALL entry — set by scheduler on context switch */
uint64_t syscall_kernel_rsp;

/* --- Built-in syscall handlers --- */

/* SYS_EXIT: terminate current thread */
static int64_t sys_exit(uint64_t status, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    kprintf("[SYSCALL] exit(%lu) from pid=%u\n", status, proc_current()->pid);
    thread_current()->state = THREAD_DEAD;
    sched_schedule();
    /* Should not return */
    for (;;) __asm__ volatile ("hlt");
    __builtin_unreachable();
}

/* SYS_WRITE: write to fd (fd 1/2 → serial output) */
static int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    /* For now, only fd 1 (stdout) and fd 2 (stderr) go to serial */
    if (fd != FD_STDOUT && fd != FD_STDERR) {
        return -EBADF;
    }

    /* TODO: validate user pointer range */
    const char *buf = (const char *)buf_addr;
    for (uint64_t i = 0; i < count; i++) {
        serial_putchar(buf[i]);
    }
    return (int64_t)count;
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

    /* TODO: validate user pointer */
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
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -ENOSYS;

    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;

    /* TODO: validate user pointer */
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

    kprintf("[SYSCALL] Initialized (LSTAR=0x%lx, STAR=0x%lx)\n",
            (uint64_t)syscall_entry, star);
}
