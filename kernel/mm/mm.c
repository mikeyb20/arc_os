/* arc_os — user address spaces: VMAs, demand paging, mmap (mm.h, M1-07)
 *
 * Owns: every struct Mm, its VMA set and the user half of its page tables;
 * the RSS counters (per mm and the AnonPages total).
 * Invariants: every user page that is mapped lies inside a VMA and matches
 * its protection (PROT_NONE pages are kept in a not-present entry, vmm.h);
 * rss counts exactly the pages mapped; a frame that leaves the page tables is
 * freed only after the TLB flush that makes it unreachable from every CPU
 * (unmapped frames and emptied tables collect on a ZapList until then).
 * Locks: mm->lock (sleeping rw, LOCK_LEVEL_MM) for writing around VMA
 * changes, for reading around faults and walks; mm->pt_lock (spinlock,
 * LOCK_LEVEL_PAGE_TABLE) inside it around page-table changes. Neither is held
 * across user memory access; the flush runs after pt_lock is dropped.
 * Cost: VMA lookup O(log n); a fault takes the read lock, one O(log n)
 * lookup and one page allocation; unmap is O(VMAs touched + page tables
 * walked) and frees emptied page tables; the mmap gap search is O(log n) from
 * its cached hint, O(VMAs passed) in the worst case. */

#include "mm/mm.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "mm/page.h"
#include "arch/hal.h"
#include "uapi/errno.h"
#include "lib/kprintf.h"

static struct kmem_cache *mm_cache;

/* User pages mapped across all address spaces (/proc/meminfo AnonPages) */
static uint64_t anon_pages_total;

/* MAP_32BIT range (Linux: the second GiB) */
#define MM_32BIT_LOW  0x40000000ULL
#define MM_32BIT_HIGH 0x80000000ULL

void mm_init(void) {
    vma_cache_init();
    mm_cache = kmem_cache_create("mm", sizeof(Mm), 64);
}

static void rss_add(Mm *mm, int64_t pages) {
    __atomic_add_fetch(&mm->rss, (uint64_t)pages, __ATOMIC_RELAXED);
    __atomic_add_fetch(&anon_pages_total, (uint64_t)pages, __ATOMIC_RELAXED);
}

uint64_t mm_rss(const Mm *mm) {
    return __atomic_load_n(&mm->rss, __ATOMIC_RELAXED);
}

uint64_t mm_anon_pages(void) {
    return __atomic_load_n(&anon_pages_total, __ATOMIC_RELAXED);
}

/* VMA protection -> page-table flags */
static uint32_t prot_to_vmm(uint32_t prot) {
    if ((prot & VMA_PROT_MASK) == 0) return VMM_FLAG_PROTNONE;
    uint32_t f = VMM_FLAG_USER;
    if (prot & VMA_WRITE) f |= VMM_FLAG_WRITABLE;
    if (!(prot & VMA_EXEC)) f |= VMM_FLAG_NOEXEC;
    return f;
}

void mm_flush_tlb_range(Mm *mm, uint64_t start, uint64_t end) {
    (void)hal_tlb_flush(&mm->cpus, mm->pgd, start, end);
}

/* --- Frames waiting for the TLB flush --- */

typedef struct ZapList {
    struct page_list pages;    /* user pages */
    struct page_list tables;   /* emptied page-table pages */
} ZapList;

static void zap_defer(uint64_t phys, int table, void *ctx) {
    ZapList *z = ctx;
    page_list_push(table ? &z->tables : &z->pages, pfn_to_page(phys >> PAGE_SHIFT));
}

static void zap_free(ZapList *z) {
    struct page *pg;
    while ((pg = page_list_first(&z->pages)) != NULL) {
        page_list_remove(&z->pages, pg);
        pmm_free_page(page_to_phys(pg));
    }
    while ((pg = page_list_first(&z->tables)) != NULL) {
        page_list_remove(&z->tables, pg);
        vmm_release_table(page_to_phys(pg));
    }
}

/* Take every page of [start, end) out of the page tables, flush (always
 * with force, else only if something was taken out), free. Holds mm->lock
 * for writing. Returns pages freed. */
static uint64_t zap_range(Mm *mm, uint64_t start, uint64_t end, int force) {
    ZapList z = {PAGE_LIST_INIT, PAGE_LIST_INIT};
    spin_lock(&mm->pt_lock);
    uint64_t n = vmm_unmap_range_in(mm->pgd, start, end, zap_defer, &z);
    spin_unlock(&mm->pt_lock);
    if (n != 0) rss_add(mm, -(int64_t)n);
    if (force || n != 0 || !page_list_empty(&z.tables)) mm_flush_tlb_range(mm, start, end);
    zap_free(&z);
    return n;
}

/* --- Lifetime --- */

Mm *mm_create(void) {
    if (mm_cache == NULL) return NULL;
    Mm *mm = kmem_cache_alloc(mm_cache, GFP_ZERO);
    if (mm == NULL) return NULL;
    mm->pgd = vmm_create_user_pml4();
    if (mm->pgd == 0) {
        kmem_cache_free(mm_cache, mm);
        return NULL;
    }
    rwsem_init(&mm->lock, LOCK_LEVEL_MM);
    spin_lock_init(&mm->pt_lock, LOCK_LEVEL_PAGE_TABLE, "mm page tables");
    vma_set_init(&mm->vmas);
    mm->users = 1;
    mm->mmap_base = PAGE_ALIGN_DOWN(USER_STACK_TOP - MM_MMAP_GAP);
    mm->mmap_hint = mm->mmap_base;
    return mm;
}

static void mm_destroy(Mm *mm) {
#if defined(ARC_DEBUG) && ARC_DEBUG
    for (int w = 0; w < CPUMASK_WORDS; w++) {
        if (__atomic_load_n(&mm->cpus.bits[w], __ATOMIC_RELAXED) != 0) {
            kprintf("[MM] FATAL: address space freed while a CPU has it loaded\n");
            KERNEL_PANIC();
        }
    }
#endif
    vmm_free_user_pages(mm->pgd);
    rss_add(mm, -(int64_t)mm_rss(mm));
    Vma *v = mm->vmas.head;
    while (v != NULL) {
        Vma *next = v->next;
        vma_free(v);
        v = next;
    }
    kmem_cache_free(mm_cache, mm);
}

void mm_get(Mm *mm) {
    __atomic_add_fetch(&mm->users, 1, __ATOMIC_RELAXED);
}

void mm_put(Mm *mm) {
    if (mm != NULL && __atomic_sub_fetch(&mm->users, 1, __ATOMIC_ACQ_REL) == 0) mm_destroy(mm);
}

void mm_switch(Mm *next) {
    /* Interrupts off throughout: a preemption here could move the thread to
     * another CPU between reading cpu and the clear, leaving a stale bit */
    uint64_t flags = irq_save();
    Mm *prev = hal_this_cpu_mm();
    if (prev != next) {
        uint32_t cpu = hal_cpu_index();
        /* In the set before CR3 loads it: a flush that misses this CPU happened
         * before, so the page walks after the switch see the new entries */
        if (next != NULL) cpumask_set_atomic(&next->cpus, cpu);
        hal_switch_address_space(next != NULL ? next->pgd : vmm_get_kernel_pml4());
        hal_set_this_cpu_mm(next);
        if (prev != NULL) cpumask_clear_atomic(&prev->cpus, cpu);
    }
    irq_restore(flags);
}

Mm *mm_fork(Mm *src) {
    Mm *dst = mm_create();
    if (dst == NULL) return NULL;
    int err = 0;
    uint64_t copied = 0;
    rwsem_down_read(&src->lock);
    for (Vma *v = src->vmas.head; v != NULL && err == 0; v = v->next) {
        Vma *nv = vma_alloc();
        if (nv == NULL) {
            err = -ENOMEM;
            break;
        }
        nv->start = v->start;
        nv->end = v->end;
        nv->prot = v->prot;
        nv->flags = v->flags;
        nv->file = v->file;
        nv->pgoff = v->pgoff;
        nv->ops = v->ops;
        vma_insert(&dst->vmas, nv);
        /* dst is private to this thread; src's pt_lock keeps concurrent
         * faults out of the tables being copied (copy-on-write: M1-08) */
        spin_lock(&src->pt_lock);
        err = vmm_copy_range_in(src->pgd, dst->pgd, v->start, v->end, &copied);
        spin_unlock(&src->pt_lock);
    }
    dst->brk_start = src->brk_start;
    dst->brk = src->brk;
    dst->mmap_base = src->mmap_base;
    dst->mmap_hint = src->mmap_hint;
    rwsem_up_read(&src->lock);
    rss_add(dst, (int64_t)copied);
    if (err != 0) {
        mm_put(dst);
        return NULL;
    }
    return dst;
}

/* --- exec / init setup (the mm is not reachable by anyone else yet) --- */

static int setup_insert(Mm *mm, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
    Vma *v = vma_alloc();
    if (v == NULL) return -ENOMEM;
    v->start = start;
    v->end = end;
    v->prot = prot & VMA_PROT_MASK;
    v->flags = flags | VMA_ANON;
    vma_insert(&mm->vmas, v);
    return 0;
}

int mm_setup_vma(Mm *mm, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
    if (start >= end || end > MM_USER_END) return -EINVAL;
    if (vma_range_free(&mm->vmas, start, end)) return setup_insert(mm, start, end, prot, flags);
    /* ELF segments may share one page with the VMA below (the end of .text
     * and the start of .data): that page becomes its own VMA with both
     * protections. Any other overlap is refused. */
    Vma *below = vma_find(&mm->vmas, start);
    if (below == NULL || below->start > start || below->end != start + PAGE_SIZE ||
        !vma_range_free(&mm->vmas, start + PAGE_SIZE, end))
        return -EINVAL;
    if (below->start < start && vma_split(&mm->vmas, below, start) == NULL) return -ENOMEM;
    Vma *shared = vma_find_containing(&mm->vmas, start);
    shared->prot |= prot & VMA_PROT_MASK;
    return end > start + PAGE_SIZE ? setup_insert(mm, start + PAGE_SIZE, end, prot, flags) : 0;
}

int mm_setup_page(Mm *mm, uint64_t virt, uint64_t phys, uint32_t vmm_flags) {
    if (vmm_map_page_in(mm->pgd, virt, phys, vmm_flags) != 0) return -ENOMEM;
    rss_add(mm, 1);
    return 0;
}

int mm_setup_stack(Mm *mm, int exec, uint64_t brk_start) {
    uint32_t prot = VMA_READ | VMA_WRITE | (exec ? VMA_EXEC : 0);
    if (!vma_range_free(&mm->vmas, USER_STACK_TOP - MM_STACK_INIT, USER_STACK_TOP)) return -ENOMEM;
    int err = setup_insert(mm, USER_STACK_TOP - MM_STACK_INIT, USER_STACK_TOP, prot, VMA_GROWSDOWN | VMA_STACK);
    if (err != 0) return err;
    mm->brk_start = PAGE_ALIGN_UP(brk_start);
    mm->brk = mm->brk_start;
    return 0;
}

/* --- Page faults --- */

/* 1 if the VMA allows the access (x86: any access right implies read) */
static int vma_allows(const Vma *v, uint32_t access) {
    if (access & MM_ACCESS_WRITE) return (v->prot & VMA_WRITE) != 0;
    if (access & MM_ACCESS_EXEC) return (v->prot & VMA_EXEC) != 0;
    return (v->prot & VMA_PROT_MASK) != 0;
}

/* Grow the stack VMA below which page lies down to page. Takes the write
 * lock (the caller dropped its read lock). Returns MM_FAULT_OK to retry. */
static int grow_stack(Mm *mm, uint64_t page) {
    int r = MM_FAULT_MAPERR;
    rwsem_down_write(&mm->lock);
    Vma *v = vma_find(&mm->vmas, page);
    if (v != NULL && v->start <= page) {
        r = MM_FAULT_OK;   /* another thread grew it */
    } else if (v != NULL && (v->flags & VMA_GROWSDOWN) && v->end - page <= MM_STACK_MAX) {
        Vma *below = v->prev;
        if (below == NULL || below->end + MM_STACK_GUARD <= page) {
            vma_set_bounds(&mm->vmas, v, page, v->end);
            r = MM_FAULT_OK;
        }
    }
    rwsem_up_write(&mm->lock);
    return r;
}

/* Map the page at addr in v (read lock held). */
static int fault_in(Mm *mm, Vma *v, uint64_t page, uint32_t access) {
    uint32_t flags = prot_to_vmm(v->prot);
    spin_lock(&mm->pt_lock);
    uint64_t old;
    if (vmm_lookup_in(mm->pgd, page, &old)) {
        /* Already there: another thread mapped it, or this CPU's TLB still
         * held an older, narrower entry. Rewrite it and drop the stale one. */
        int map_err = vmm_map_page_in(mm->pgd, page, old, flags);
        spin_unlock(&mm->pt_lock);
        hal_invalidate_page(page);
        return map_err == 0 ? MM_FAULT_OK : MM_FAULT_OOM;
    }
    spin_unlock(&mm->pt_lock);

    uint64_t phys = 0;
    if (v->ops != NULL && v->ops->fault != NULL) {
        if (v->ops->fault(v, page, access, &phys) != 0) return MM_FAULT_OOM;
    } else {
        struct page *pg = alloc_pages(0, GFP_ZERO);
        if (pg == NULL) return MM_FAULT_OOM;
        phys = page_to_phys(pg);
    }

    spin_lock(&mm->pt_lock);
    if (vmm_lookup_in(mm->pgd, page, NULL)) {
        spin_unlock(&mm->pt_lock);   /* lost a race: theirs is as good */
        pmm_free_page(phys);
        return MM_FAULT_OK;
    }
    int err = vmm_map_page_in(mm->pgd, page, phys, flags);
    spin_unlock(&mm->pt_lock);
    if (err != 0) {
        pmm_free_page(phys);
        return MM_FAULT_OOM;
    }
    rss_add(mm, 1);
    return MM_FAULT_OK;
}

int mm_fault(Mm *mm, uint64_t addr, uint32_t access) {
    if (addr >= MM_USER_END) return MM_FAULT_MAPERR;
    uint64_t page = PAGE_ALIGN_DOWN(addr);
    for (;;) {
        rwsem_down_read(&mm->lock);
        Vma *v = vma_find(&mm->vmas, page);
        if (v == NULL) {
            rwsem_up_read(&mm->lock);
            return MM_FAULT_MAPERR;
        }
        if (page < v->start) {
            int grows = (v->flags & VMA_GROWSDOWN) != 0;
            rwsem_up_read(&mm->lock);
            if (!grows || grow_stack(mm, page) != MM_FAULT_OK) return MM_FAULT_MAPERR;
            continue;   /* look again: the VMA may have changed meanwhile */
        }
        int r = vma_allows(v, access) ? fault_in(mm, v, page, access) : MM_FAULT_ACCERR;
        rwsem_up_read(&mm->lock);
        return r;
    }
}

/* --- Changing the map (write lock held) --- */

/* Make start a VMA boundary (split the VMA across it). 0 or -ENOMEM. */
static int split_at(Mm *mm, uint64_t addr) {
    Vma *v = vma_find(&mm->vmas, addr);
    if (v == NULL || v->start >= addr) return 0;
    return vma_split(&mm->vmas, v, addr) != NULL ? 0 : -ENOMEM;
}

/* munmap with the write lock held */
static int unmap_locked(Mm *mm, uint64_t start, uint64_t end) {
    if (split_at(mm, start) != 0 || split_at(mm, end) != 0) return -ENOMEM;
    Vma *v = vma_find(&mm->vmas, start);
    int removed = 0;
    while (v != NULL && v->start < end) {
        Vma *next = v->next;
        vma_remove(&mm->vmas, v);
        vma_free(v);
        removed = 1;
        v = next;
    }
    if (!removed) return 0;
    zap_range(mm, start, end, 0);
    /* The free-area cache: the next search may use this space again */
    if (end > mm->mmap_hint) mm->mmap_hint = end < mm->mmap_base ? end : mm->mmap_base;
    return 0;
}

static int range_mapped_locked(Mm *mm, uint64_t start, uint64_t end) {
    uint64_t a = start;
    Vma *v = vma_find(&mm->vmas, a);
    while (a < end) {
        if (v == NULL || v->start > a) return 0;
        a = v->end;
        v = v->next;
    }
    return 1;
}

/* Top-down search for len free bytes, starting at the cached hint */
static uint64_t find_free(Mm *mm, uint64_t len, int below_2g) {
    if (below_2g) return vma_find_gap(&mm->vmas, len, MM_32BIT_LOW, MM_32BIT_HIGH, MM_32BIT_HIGH);
    uint64_t a = vma_find_gap(&mm->vmas, len, MM_MMAP_MIN, mm->mmap_base, mm->mmap_hint);
    if (a == 0 && mm->mmap_hint < mm->mmap_base)
        a = vma_find_gap(&mm->vmas, len, MM_MMAP_MIN, mm->mmap_base, mm->mmap_base);
    if (a != 0) mm->mmap_hint = a;
    return a;
}

int64_t mm_map_anon(Mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, int how, int populate,
                    int below_2g) {
    if (len == 0 || len > MM_USER_END) return -ENOMEM;
    rwsem_down_write(&mm->lock);
    uint64_t start = 0;
    if (how == MM_MAP_HINT) {
        uint64_t hint = PAGE_ALIGN_UP(addr);
        if (hint >= MM_MMAP_MIN && hint <= MM_USER_END - len && vma_range_free(&mm->vmas, hint, hint + len))
            start = hint;
        if (start == 0) start = find_free(mm, len, below_2g);
        if (start == 0) {
            rwsem_up_write(&mm->lock);
            return -ENOMEM;
        }
    } else {
        start = addr;
        if (how == MM_MAP_NOREPLACE && !vma_range_free(&mm->vmas, start, start + len)) {
            rwsem_up_write(&mm->lock);
            return -EEXIST;
        }
        if (unmap_locked(mm, start, start + len) != 0) {
            rwsem_up_write(&mm->lock);
            return -ENOMEM;
        }
    }
    Vma *v = vma_alloc();
    if (v == NULL) {
        rwsem_up_write(&mm->lock);
        return -ENOMEM;
    }
    v->start = start;
    v->end = start + len;
    v->prot = prot & VMA_PROT_MASK;
    v->flags = flags | VMA_ANON;
    vma_insert(&mm->vmas, v);
    (void)vma_merge_around(&mm->vmas, v);
    rwsem_up_write(&mm->lock);

    if (populate && (prot & VMA_PROT_MASK) != 0) {
        uint32_t access = (prot & VMA_WRITE) ? MM_ACCESS_WRITE : 0;
        for (uint64_t a = start; a < start + len; a += PAGE_SIZE) {
            if (mm_fault(mm, a, access) != MM_FAULT_OK) break;   /* MAP_POPULATE is best effort */
        }
    }
    return (int64_t)start;
}

int mm_unmap(Mm *mm, uint64_t addr, uint64_t len) {
    rwsem_down_write(&mm->lock);
    int err = unmap_locked(mm, addr, addr + len);
    rwsem_up_write(&mm->lock);
    return err;
}

int mm_protect(Mm *mm, uint64_t addr, uint64_t len, uint32_t prot) {
    uint64_t end = addr + len;
    rwsem_down_write(&mm->lock);
    if (!range_mapped_locked(mm, addr, end)) {
        rwsem_up_write(&mm->lock);
        return -ENOMEM;
    }
    if (split_at(mm, addr) != 0 || split_at(mm, end) != 0) {
        rwsem_up_write(&mm->lock);
        return -ENOMEM;
    }
    for (Vma *v = vma_find(&mm->vmas, addr); v != NULL && v->start < end; v = v->next) v->prot = prot & VMA_PROT_MASK;
    spin_lock(&mm->pt_lock);
    vmm_protect_range_in(mm->pgd, addr, end, prot_to_vmm(prot));
    spin_unlock(&mm->pt_lock);
    mm_flush_tlb_range(mm, addr, end);
    /* Merge what now matches; each step keeps the survivor */
    Vma *v = vma_find(&mm->vmas, addr ? addr - 1 : 0);
    while (v != NULL && v->start <= end) {
        v = vma_merge_around(&mm->vmas, v);
        v = v->next;
    }
    rwsem_up_write(&mm->lock);
    return 0;
}

int mm_dontneed(Mm *mm, uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;
    rwsem_down_write(&mm->lock);
    int mapped = range_mapped_locked(mm, addr, end);
    zap_range(mm, addr, end, 0);   /* holes have no pages: zapping them is a no-op */
    rwsem_up_write(&mm->lock);
    return mapped ? 0 : -ENOMEM;
}

int mm_range_mapped(Mm *mm, uint64_t addr, uint64_t len) {
    rwsem_down_read(&mm->lock);
    int r = range_mapped_locked(mm, addr, addr + len);
    rwsem_up_read(&mm->lock);
    return r;
}

/* Move [old, old + old_len) (inside one VMA) to dest, as a VMA of new_len */
static int64_t move_locked(Mm *mm, uint64_t old, uint64_t old_len, uint64_t new_len, uint64_t dest) {
    if (split_at(mm, old) != 0 || split_at(mm, old + old_len) != 0) return -ENOMEM;
    Vma *src = vma_find_containing(&mm->vmas, old);
    Vma *nv = vma_alloc();
    if (nv == NULL) return -ENOMEM;
    nv->start = dest;
    nv->end = dest + new_len;
    nv->prot = src->prot;
    nv->flags = src->flags & ~(VMA_HEAP | VMA_STACK | VMA_GROWSDOWN);   /* moved: no longer the heap/stack */
    nv->file = src->file;
    nv->pgoff = src->pgoff;
    nv->ops = src->ops;

    spin_lock(&mm->pt_lock);
    int err = vmm_move_range_in(mm->pgd, old, dest, old_len);
    if (err != 0) (void)vmm_move_range_in(mm->pgd, dest, old, old_len);   /* old's tables exist: can't fail */
    spin_unlock(&mm->pt_lock);
    if (err != 0) {
        zap_range(mm, dest, dest + old_len, 1);   /* flush dest, free the tables the move added */
        vma_free(nv);
        return -ENOMEM;
    }
    vma_remove(&mm->vmas, src);
    vma_free(src);
    vma_insert(&mm->vmas, nv);
    (void)vma_merge_around(&mm->vmas, nv);
    /* No pages are left at old: flush its entries, free the tables the
     * move emptied */
    zap_range(mm, old, old + old_len, 1);
    return (int64_t)dest;
}

int64_t mm_remap(Mm *mm, uint64_t old_addr, uint64_t old_len, uint64_t new_len, int may_move, int fixed,
                 uint64_t new_addr) {
    int64_t r;
    rwsem_down_write(&mm->lock);
    Vma *v = vma_find_containing(&mm->vmas, old_addr);
    if (v == NULL || old_addr + old_len > v->end) {
        r = -EFAULT;
        goto out;
    }
    if (fixed) {
        if (new_addr < old_addr + old_len && old_addr < new_addr + new_len) {
            r = -EINVAL;
            goto out;
        }
        if (unmap_locked(mm, new_addr, new_addr + new_len) != 0) {
            r = -ENOMEM;
            goto out;
        }
        if (new_len < old_len) {
            if (unmap_locked(mm, old_addr + new_len, old_addr + old_len) != 0) {
                r = -ENOMEM;
                goto out;
            }
            old_len = new_len;
        }
        r = move_locked(mm, old_addr, old_len, new_len, new_addr);
        goto out;
    }
    if (new_len <= old_len) {
        r = unmap_locked(mm, old_addr + new_len, old_addr + old_len);
        if (r == 0) r = (int64_t)old_addr;
        goto out;
    }
    /* Grow in place when the range ends its VMA and the space above is free */
    if (old_addr + old_len == v->end && old_addr + new_len <= MM_USER_END &&
        vma_range_free(&mm->vmas, v->end, old_addr + new_len)) {
        vma_set_bounds(&mm->vmas, v, v->start, old_addr + new_len);
        (void)vma_merge_around(&mm->vmas, v);
        r = (int64_t)old_addr;
        goto out;
    }
    if (!may_move) {
        r = -ENOMEM;
        goto out;
    }
    uint64_t dest = find_free(mm, new_len, 0);
    r = dest != 0 ? move_locked(mm, old_addr, old_len, new_len, dest) : -ENOMEM;
out:
    rwsem_up_write(&mm->lock);
    return r;
}

uint64_t mm_brk(Mm *mm, uint64_t new_brk) {
    rwsem_down_write(&mm->lock);
    uint64_t cur = mm->brk;
    if (new_brk < mm->brk_start || new_brk >= MM_USER_END) goto out;
    uint64_t old_top = PAGE_ALIGN_UP(cur);
    uint64_t new_top = PAGE_ALIGN_UP(new_brk);
    if (new_top > old_top) {
        /* Linux keeps one free page above the heap */
        if (!vma_range_free(&mm->vmas, old_top, new_top + PAGE_SIZE)) goto out;
        Vma *heap = old_top > mm->brk_start ? vma_find_containing(&mm->vmas, old_top - 1) : NULL;
        if (heap != NULL && (heap->flags & VMA_HEAP) && heap->end == old_top) {
            vma_set_bounds(&mm->vmas, heap, heap->start, new_top);
        } else {
            Vma *v = vma_alloc();
            if (v == NULL) goto out;
            v->start = old_top;
            v->end = new_top;
            v->prot = VMA_READ | VMA_WRITE;
            v->flags = VMA_ANON | VMA_HEAP;
            vma_insert(&mm->vmas, v);
            (void)vma_merge_around(&mm->vmas, v);
        }
    } else if (new_top < old_top) {
        if (unmap_locked(mm, new_top, old_top) != 0) goto out;
    }
    mm->brk = new_brk;
    cur = new_brk;
out:
    rwsem_up_write(&mm->lock);
    return cur;
}

/* --- Reporting --- */

/* Linux pads the fields before a name to this width, then adds a space */
#define MAPS_NAME_WIDTH 72

int mm_format_maps(Mm *mm, char *buf, int size) {
    int len = 0;
    rwsem_down_read(&mm->lock);
    for (Vma *v = mm->vmas.head; v != NULL && len < size; v = v->next) {
        const char *name = (v->flags & VMA_HEAP) ? "[heap]" : (v->flags & VMA_STACK) ? "[stack]" : NULL;
        int n =
            ksnprintf(buf + len, (size_t)(size - len), "%08lx-%08lx %c%c%c%c %08lx 00:00 0 ", v->start, v->end,
                      (v->prot & VMA_READ) ? 'r' : '-', (v->prot & VMA_WRITE) ? 'w' : '-',
                      (v->prot & VMA_EXEC) ? 'x' : '-', (v->flags & VMA_SHARED) ? 's' : 'p', v->pgoff << PAGE_SHIFT);
        if (n < 0 || n >= size - len) break;
        if (name != NULL) {
            int pad = (n < MAPS_NAME_WIDTH ? MAPS_NAME_WIDTH - n : 0) + 1;
            if (len + n + pad >= size) break;
            for (int i = 0; i < pad; i++) buf[len + n++] = ' ';
            n += ksnprintf(buf + len + n, (size_t)(size - len - n), "%s", name);
        }
        if (n >= size - len - 1) break;   /* room for the newline */
        buf[len + n] = '\n';
        len += n + 1;
    }
    rwsem_up_read(&mm->lock);
    if (len < size) buf[len] = '\0';
    return len;
}

uint64_t mm_reap(Mm *mm) {
    rwsem_down_write(&mm->lock);
    uint64_t freed = 0;
    for (Vma *v = mm->vmas.head; v != NULL; v = v->next) freed += zap_range(mm, v->start, v->end, 0);
    rwsem_up_write(&mm->lock);
    return freed;
}
