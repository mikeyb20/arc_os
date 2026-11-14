/* arc_os — memory syscalls: mmap, munmap, mprotect, madvise, mremap, brk
 * (Linux x86_64 numbers and semantics, ADR-0001; M1-07)
 *
 * Owns: argument checking and Linux's errno choices; the work is in mm.c.
 * Locks: none here (mm.c takes the mm lock).
 * Cost: O(1) checks, then mm.c's cost. */

#include "mm/mm_syscalls.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "proc/process.h"
#include "proc/fd.h"
#include "arch/hal.h"
#include "uapi/errno.h"
#include "uapi/mman.h"
#include "uapi/unistd.h"

static Mm *current_mm(void) {
    Process *p = proc_current();
    return p != NULL ? p->mm : NULL;
}

static int page_aligned(uint64_t v) {
    return (v & (PAGE_SIZE - 1)) == 0;
}

/* Round len up to pages; 0 if that overflows or leaves user space */
static uint64_t page_len(uint64_t len) {
    if (len > MM_USER_END) return 0;
    return PAGE_ALIGN_UP(len);
}

/* mmap(addr, len, prot, flags, fd, offset) — 9. Anonymous private memory;
 * file mappings come with the page cache (M1-12) and return -ENODEV. */
static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
    Mm *mm = current_mm();
    if (mm == NULL) return -ENOMEM;
    if (!page_aligned(offset)) return -EINVAL;
    if (len == 0) return -EINVAL;
    len = page_len(len);
    if (len == 0) return -ENOMEM;
    if ((offset >> 12) + (len >> 12) < (offset >> 12)) return -EOVERFLOW;

    uint64_t type = flags & MAP_TYPE;
    if (type != MAP_PRIVATE && type != MAP_SHARED && type != MAP_SHARED_VALIDATE) return -EINVAL;
    if (!(flags & MAP_ANONYMOUS)) {
        File *f = (int64_t)fd < 0 ? NULL : fd_get(proc_current()->fd_table, (int)fd);
        if (f == NULL) return -EBADF;
        fput(f);
        return -ENODEV;   /* file mappings: M1-12 */
    }
    if (type != MAP_PRIVATE) return -EINVAL;   /* shared anonymous memory needs refcounted pages (M1-08) */

    int how = MM_MAP_HINT;
    if (flags & MAP_FIXED_NOREPLACE)
        how = MM_MAP_NOREPLACE;
    else if (flags & MAP_FIXED)
        how = MM_MAP_FIXED;
    if (how != MM_MAP_HINT) {
        if (!page_aligned(addr)) return -EINVAL;
        if (addr < MM_MMAP_MIN) return -EPERM;
        if (addr > MM_USER_END - len) return -ENOMEM;
    }
    uint32_t vflags = (flags & MAP_GROWSDOWN) ? VMA_GROWSDOWN : 0;
    return mm_map_anon(mm, addr, len, (uint32_t)prot & VMA_PROT_MASK, vflags, how, (flags & MAP_POPULATE) != 0,
                       (flags & MAP_32BIT) != 0);
}

/* munmap(addr, len) — 11 */
static int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    Mm *mm = current_mm();
    if (mm == NULL) return -EINVAL;
    if (!page_aligned(addr) || len == 0) return -EINVAL;
    len = page_len(len);
    if (len == 0 || addr > MM_USER_END - len) return -EINVAL;
    return mm_unmap(mm, addr, len);
}

/* mprotect(addr, len, prot) — 10 */
static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3;
    (void)a4;
    (void)a5;
    Mm *mm = current_mm();
    if (mm == NULL) return -ENOMEM;
    if (!page_aligned(addr)) return -EINVAL;
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_SEM | PROT_GROWSDOWN | PROT_GROWSUP))
        return -EINVAL;
    if (prot & (PROT_GROWSDOWN | PROT_GROWSUP)) return -EINVAL;   /* not supported */
    if (len == 0) return 0;
    len = page_len(len);
    if (len == 0 || addr > MM_USER_END - len) return -ENOMEM;
    return mm_protect(mm, addr, len, (uint32_t)prot & VMA_PROT_MASK);
}

/* madvise(addr, len, advice) — 28. DONTNEED and FREE drop the pages now;
 * the other hints are accepted and change nothing yet. */
static int64_t sys_madvise(uint64_t addr, uint64_t len_in, uint64_t advice, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3;
    (void)a4;
    (void)a5;
    Mm *mm = current_mm();
    if (mm == NULL) return -ENOMEM;
    if (!page_aligned(addr)) return -EINVAL;
    uint64_t len = page_len(len_in);
    if (len_in != 0 && len == 0) return -EINVAL;
    if (addr > MM_USER_END - len) return -EINVAL;
    switch (advice) {
    case MADV_DONTNEED:
    case MADV_FREE:
        if (len == 0) return 0;
        return mm_dontneed(mm, addr, len);
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
    case MADV_HUGEPAGE:
    case MADV_NOHUGEPAGE:
        if (len == 0) return 0;
        return mm_range_mapped(mm, addr, len) ? 0 : -ENOMEM;
    default:
        return -EINVAL;
    }
}

/* mremap(old_addr, old_len, new_len, flags, new_addr) — 25 */
static int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr,
                          uint64_t a5) {
    (void)a5;
    Mm *mm = current_mm();
    if (mm == NULL) return -ENOMEM;
    if (!page_aligned(old_addr)) return -EINVAL;
    if (flags & ~(uint64_t)(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP)) return -EINVAL;
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE)) return -EINVAL;
    if (flags & MREMAP_DONTUNMAP) return -EINVAL;   /* not supported */
    old_len = page_len(old_len);
    new_len = page_len(new_len);
    if (new_len == 0) return -EINVAL;
    if (old_len == 0) return -EINVAL;   /* duplicating needs shared mappings */
    if (old_addr > MM_USER_END - old_len) return -EFAULT;
    int fixed = (flags & MREMAP_FIXED) != 0;
    if (fixed && (!page_aligned(new_addr) || new_addr > MM_USER_END - new_len)) return -EINVAL;
    return mm_remap(mm, old_addr, old_len, new_len, (flags & MREMAP_MAYMOVE) != 0, fixed, new_addr);
}

/* brk(addr) — 12. Linux semantics: returns the new break on success and the
 * unchanged break on any failure (never -errno); brk(0) queries it. The heap
 * is a VMA; its pages come on first touch. */
static int64_t sys_brk(uint64_t new_brk, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    Mm *mm = current_mm();
    if (mm == NULL) return 0;
    return (int64_t)mm_brk(mm, new_brk);
}

void mm_syscalls_init(void) {
    syscall_register(__NR_mmap, sys_mmap);
    syscall_register(__NR_mprotect, sys_mprotect);
    syscall_register(__NR_munmap, sys_munmap);
    syscall_register(__NR_brk, sys_brk);
    syscall_register(__NR_mremap, sys_mremap);
    syscall_register(__NR_madvise, sys_madvise);
}
