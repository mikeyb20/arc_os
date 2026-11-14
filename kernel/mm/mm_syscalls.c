/* arc_os — memory syscalls: mmap, munmap, mprotect, madvise, mremap, msync,
 * brk (Linux x86_64 numbers and semantics, ADR-0001; M1-07, file mappings
 * M1-12)
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
#include "uapi/fcntl.h"
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

/* The address choice of mmap's flags (MM_MAP_*), or -errno for a bad fixed address */
static int64_t map_how(uint64_t addr, uint64_t len, uint64_t flags) {
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
    return how;
}

/* A file mapping (Linux's checks in do_mmap): the file must be open for
 * reading; a shared writable mapping needs it open for writing too, and a
 * shared mapping of a read-only open can never be made writable. Files
 * without mmap_ops (directories, most devices) give -ENODEV. */
static int64_t map_file(Mm *mm, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int how, uint64_t fd,
                        uint64_t offset) {
    File *f = (int64_t)fd < 0 ? NULL : fd_get(proc_current()->fd_table, (int)fd);
    if (f == NULL) return -EBADF;
    uint32_t mode = f->flags & O_ACCMODE;
    int shared = (flags & MAP_TYPE) != MAP_PRIVATE;
    int64_t r;
    if (f->fop == NULL || f->fop->mmap_ops == NULL) {
        r = -ENODEV;
    } else if (mode == O_WRONLY) {
        r = -EACCES;
    } else if (shared && (prot & PROT_WRITE) && mode != O_RDWR) {
        r = -EACCES;
    } else {
        uint32_t vflags = shared ? VMA_SHARED : 0;
        if (!shared || mode == O_RDWR) vflags |= VMA_MAYWRITE;
        r = mm_map_object(mm, addr, len, (uint32_t)prot & VMA_PROT_MASK, vflags, how, (flags & MAP_POPULATE) != 0, f,
                          offset >> PAGE_SHIFT, f->fop->mmap_ops);   /* the VMA takes its own File reference */
    }
    fput(f);
    return r;
}

/* mmap(addr, len, prot, flags, fd, offset) — 9. Anonymous private memory,
 * and files whose data is in the page cache (private or shared). */
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
    int64_t how = map_how(addr, len, flags);
    if (how < 0) return how;
    if (!(flags & MAP_ANONYMOUS)) return map_file(mm, addr, len, prot, flags, (int)how, fd, offset);
    if (type != MAP_PRIVATE) return -EINVAL;   /* shared anonymous memory needs refcounted pages (M1-08) */

    uint32_t vflags = (flags & MAP_GROWSDOWN) ? VMA_GROWSDOWN : 0;
    return mm_map_anon(mm, addr, len, (uint32_t)prot & VMA_PROT_MASK, vflags, (int)how, (flags & MAP_POPULATE) != 0,
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
 * NORMAL, RANDOM and SEQUENTIAL set the file readahead of the range;
 * WILLNEED reads its file pages in now; HUGEPAGE and NOHUGEPAGE are
 * accepted and change nothing. */
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
        return len == 0 ? 0 : mm_advise_access(mm, addr, len, 0);
    case MADV_RANDOM:
        return len == 0 ? 0 : mm_advise_access(mm, addr, len, VMA_RAND_READ);
    case MADV_SEQUENTIAL:
        return len == 0 ? 0 : mm_advise_access(mm, addr, len, VMA_SEQ_READ);
    case MADV_WILLNEED:
        return len == 0 ? 0 : mm_willneed(mm, addr, len);
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

/* msync(addr, len, flags) — 26. MS_SYNC writes the range's shared file
 * pages back and waits; MS_ASYNC starts nothing (as on Linux since 2.6.19:
 * dirty pages are written by msync(MS_SYNC), fsync, and M1-14's flusher);
 * MS_INVALIDATE has nothing to drop (the cache is the only copy). */
static int64_t sys_msync(uint64_t addr, uint64_t len_in, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3;
    (void)a4;
    (void)a5;
    Mm *mm = current_mm();
    if (mm == NULL) return -ENOMEM;
    if (flags & ~(uint64_t)(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) return -EINVAL;
    if (!page_aligned(addr)) return -EINVAL;
    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) return -EINVAL;
    uint64_t len = page_len(len_in);
    if (len_in != 0 && len == 0) return -ENOMEM;
    if (addr > MM_USER_END - len) return -ENOMEM;
    if (len == 0) return 0;
    return mm_msync(mm, addr, len, (flags & MS_SYNC) != 0);
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
    syscall_register(__NR_msync, sys_msync);
}
