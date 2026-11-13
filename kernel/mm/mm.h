#ifndef ARCHOS_MM_MM_H
#define ARCHOS_MM_MM_H

/* User address spaces (M1-07, ADR-0004).
 *
 * A struct Mm is one process's address space: its page-table root, its VMAs
 * (vma.h), its heap break and the set of CPUs that have it loaded (TLB
 * shootdown). User memory is allocated on first touch: mmap only records a
 * VMA; the page-fault path (mm_fault) maps a zeroed page when the program
 * first uses an address, and grows the stack VMA downwards.
 *
 * Locks (docs/kernel/locking.md): `lock` is the mm lock, a sleeping
 * read-write lock at LOCK_LEVEL_MM. Changes to the VMAs (mmap, munmap,
 * mprotect, mremap, brk, madvise, stack growth) hold it for writing; page
 * faults, fork's copy and /proc/<pid>/maps hold it for reading. `pt_lock`
 * (LOCK_LEVEL_PAGE_TABLE) is taken inside it around every page-table change.
 * Never touch user memory while holding either: the fault would take `lock`
 * again. The TLB is flushed after `pt_lock` is released and before the freed
 * pages go back to the allocator. */

#include <stdint.h>
#include "mm/vma.h"
#include "proc/rwsem.h"
#include "proc/spinlock.h"
#include "proc/cpumask.h"

/* Address space layout (Linux x86_64 defaults) */
#define MM_USER_END    0x00007FFFFFFFF000ULL   /* TASK_SIZE: mappings end below */
#define MM_MMAP_MIN    0x10000ULL              /* vm.mmap_min_addr */
#define MM_STACK_MAX   (8ULL << 20)            /* RLIMIT_STACK default: 8 MiB */
#define MM_STACK_INIT  (128ULL << 10)          /* first stack VMA: 128 KiB */
#define MM_STACK_GUARD (1ULL << 20)            /* stack_guard_gap: 256 pages */
#define MM_MMAP_GAP    (128ULL << 20)          /* mmap_base sits this far below the stack top */

typedef struct Mm {
    uint64_t pgd;          /* page-table root (PML4 physical address) */
    RwSem lock;            /* the mm lock (see above) */
    Spinlock pt_lock;      /* page-table changes, inside `lock` */
    VmaSet vmas;
    uint64_t brk_start;    /* first byte of the heap (page aligned) */
    uint64_t brk;          /* current break */
    uint64_t mmap_base;    /* top of the top-down mmap area */
    uint64_t mmap_hint;    /* next top-down search starts here (free-area cache) */
    uint64_t rss;          /* user pages mapped (atomic) */
    uint32_t users;        /* references (atomic): the owning process, plus a
                            * holder that may sleep while it reads another
                            * process's mm (OOM reaper, /proc/<pid>/maps) */
    CpuMask cpus;          /* CPUs that have pgd loaded (atomic bits) */
} Mm;

/* Results of mm_fault */
#define MM_FAULT_OK     0
#define MM_FAULT_MAPERR 1   /* no VMA: SIGSEGV, SEGV_MAPERR */
#define MM_FAULT_ACCERR 2   /* VMA forbids the access: SIGSEGV, SEGV_ACCERR */
#define MM_FAULT_OOM    3   /* no page (or page table) could be had */
#define MM_FAULT_KILLED 4   /* mm_handle_fault: SIGKILL is pending (OOM), nothing mapped */

/* Access bits for mm_fault */
#define MM_ACCESS_WRITE   (1u << 0)
#define MM_ACCESS_EXEC    (1u << 1)
#define MM_ACCESS_PRESENT (1u << 2)   /* the page was there: a protection fault */

/* How mm_map_anon picks the address */
#define MM_MAP_HINT      0   /* addr is a hint (0 = none) */
#define MM_MAP_FIXED     1   /* exactly addr, replacing what is there */
#define MM_MAP_NOREPLACE 2   /* exactly addr, -EEXIST if anything is there */

/* Boot: the VMA and mm slab caches. */
void mm_init(void);

/* A new, empty address space (kernel half mapped), one reference. NULL when
 * out of memory. */
Mm *mm_create(void);

/* Take a reference (the caller already holds one, or holds the kernel lock
 * while the owner can't drop its own). */
void mm_get(Mm *mm);

/* Drop a reference; the last one frees the address space: its pages, page
 * tables, VMAs. No CPU may have it loaded then (mm_switch away first).
 * NULL is ignored. */
void mm_put(Mm *mm);

/* Fork: a copy of src with one reference (every VMA, every page copied;
 * copy-on-write is M1-08). NULL when out of memory. Takes src's lock for reading. */
Mm *mm_fork(Mm *src);

/* Load mm on this CPU (NULL: the kernel's page tables) and keep the "loaded
 * on" sets right. Interrupts off. */
void mm_switch(Mm *mm);

/* exec/init only, on an mm no other thread can reach yet: add an anonymous
 * VMA (it may share its first page with the VMA below, as ELF segments do:
 * that page gets both protections), and map a page the loader filled in
 * (counted in the RSS). Return 0, -EINVAL (bad or overlapping range) or
 * -ENOMEM. */
int mm_setup_vma(Mm *mm, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);
int mm_setup_page(Mm *mm, uint64_t virt, uint64_t phys, uint32_t vmm_flags);

/* exec/init: the stack VMA below USER_STACK_TOP, growing down to
 * MM_STACK_MAX; exec != 0 makes it executable (PT_GNU_STACK). Also sets the
 * heap start (page-aligned brk_start). Returns 0 or -ENOMEM. */
int mm_setup_stack(Mm *mm, int exec, uint64_t brk_start);

/* Resolve a page fault at addr (any address; access = MM_ACCESS_*). Takes the
 * mm lock. Returns MM_FAULT_*. */
int mm_fault(Mm *mm, uint64_t addr, uint32_t access);

/* The page-fault entry for the current process (arch fault handler, mm/fault.c):
 * mm_fault, and when memory runs out the OOM killer: the process with the
 * largest RSS gets SIGKILL and its pages are reaped at once, then the fault is
 * retried. Never panics. Returns MM_FAULT_OK, _MAPERR, _ACCERR, or _KILLED when
 * the current process is the one killed (or was already dying). */
int mm_handle_fault(uint64_t addr, uint32_t access);

/* mmap of anonymous memory: len (page aligned, non-zero), prot VMA_*,
 * flags VMA_* (VMA_ANON implied), how MM_MAP_*, populate: fault every page in
 * now (MAP_POPULATE), below_2g: MAP_32BIT. Returns the address or -errno. */
int64_t mm_map_anon(Mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, int how, int populate,
                    int below_2g);

/* munmap of [addr, addr + len) (page aligned). Holes are fine. 0 or -ENOMEM
 * (a VMA could not be split). */
int mm_unmap(Mm *mm, uint64_t addr, uint64_t len);

/* mprotect: every page of the range must be mapped (-ENOMEM otherwise). */
int mm_protect(Mm *mm, uint64_t addr, uint64_t len, uint32_t prot);

/* madvise(MADV_DONTNEED): drop the pages of [addr, addr + len); they read as
 * zero next time. -ENOMEM if part of the range is unmapped (the mapped part is
 * still dropped, as Linux). */
int mm_dontneed(Mm *mm, uint64_t addr, uint64_t len);

/* 1 if every page of [addr, addr + len) is inside a VMA. */
int mm_range_mapped(Mm *mm, uint64_t addr, uint64_t len);

/* mremap (MREMAP_MAYMOVE as may_move, MREMAP_FIXED as fixed with new_addr).
 * Returns the new address or -errno. */
int64_t mm_remap(Mm *mm, uint64_t old_addr, uint64_t old_len, uint64_t new_len, int may_move, int fixed,
                 uint64_t new_addr);

/* brk: move the break to new_brk if it can; returns the break afterwards. */
uint64_t mm_brk(Mm *mm, uint64_t new_brk);

/* /proc/<pid>/maps text (Linux format) into buf. Returns its length. */
int mm_format_maps(Mm *mm, char *buf, int size);

/* User pages mapped by mm (its RSS). */
uint64_t mm_rss(const Mm *mm);

/* User pages mapped by all address spaces (/proc/meminfo AnonPages). */
uint64_t mm_anon_pages(void);

/* Drop every page of a dying process's address space now (OOM kill): the
 * VMAs stay, the memory goes back to the allocator. Returns pages freed. */
uint64_t mm_reap(Mm *mm);

/* Flush [start, end) of mm from every CPU's TLB (tlb.h). */
void mm_flush_tlb_range(Mm *mm, uint64_t start, uint64_t end);

#endif /* ARCHOS_MM_MM_H */
