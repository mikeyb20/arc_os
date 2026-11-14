/* arc_os — file mappings: page faults on file VMAs, the list of VMAs that
 * map a file, and what truncate and write-back do through it
 * (filemap.h, M1-12)
 *
 * Owns: filemap_vma_ops (the VmaOps of every file VMA) and each mapping's
 * VMA list.
 * Invariants: a file VMA holds a File reference and is on its mapping's list
 * from ops->open to ops->close; a cache page is mapped read-only except in a
 * shared VMA after a write fault marked it dirty (so the first store to a
 * clean page always faults); a private VMA never has a cache page writable:
 * a write copies it into an anonymous page first; mapcount and the page
 * references follow the entries exactly (the count changes under the mm's
 * page-table lock with the entry).
 * Locks: faults hold the mm lock for reading, then the page lock, then the
 * page-table lock. Truncate and write-back hold the page lock or the inode
 * lock, then the mapping lock, then each mm's page-table lock; they never
 * take an mm lock (the VMAs on the list are alive while the mapping lock is
 * held: ops->close waits for it).
 * Cost: a fault on a cached page copies nothing (one lookup, one entry); a
 * private write copies one page; truncate, write-protect and reclaim's
 * reverse mapping (filemap_page_young, filemap_unmap_page) walk the
 * mapping's VMAs (one per mapping process, typically a handful). */

#include "mm/filemap.h"
#include "mm/mm.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/mem.h"
#include "uapi/errno.h"

/* --- The list of VMAs mapping a file --- */

static AddressSpace *vma_mapping(const Vma *v) {
    return filemap_of(((File *)v->file)->inode);
}

static void file_vma_open(Vma *v) {
    fget_ref(v->file);
    AddressSpace *as = vma_mapping(v);
    mutex_lock(&as->map_lock);
    v->as_prev = NULL;
    v->as_next = as->vmas;
    if (as->vmas != NULL) as->vmas->as_prev = v;
    as->vmas = v;
    mutex_unlock(&as->map_lock);
}

static void file_vma_close(Vma *v) {
    AddressSpace *as = vma_mapping(v);
    mutex_lock(&as->map_lock);
    if (v->as_prev != NULL)
        v->as_prev->as_next = v->as_next;
    else
        as->vmas = v->as_next;
    if (v->as_next != NULL) v->as_next->as_prev = v->as_prev;
    v->as_next = v->as_prev = NULL;
    mutex_unlock(&as->map_lock);
    fput(v->file);   /* may free the inode and its mapping: after the unlock */
}

/* Page index of the first page v maps, and one past its last */
static uint64_t vma_first(const Vma *v) {
    return v->pgoff;
}
static uint64_t vma_end(const Vma *v) {
    return v->pgoff + ((v->end - v->start) >> PAGE_SHIFT);
}

void filemap_unmap_from(AddressSpace *as, uint64_t first) {
    mutex_lock(&as->map_lock);
    for (Vma *v = as->vmas; v != NULL; v = v->as_next) {
        if (vma_end(v) <= first) continue;
        uint64_t from = first > vma_first(v) ? first : vma_first(v);
        mm_zap_range(v->mm, v->start + ((from - v->pgoff) << PAGE_SHIFT), v->end, MM_ZAP_FLUSH);
    }
    mutex_unlock(&as->map_lock);
}

void filemap_unmap_page(AddressSpace *as, struct page *pg) {
    uint64_t index = pg->index;
    uint64_t phys = page_to_phys(pg);
    mutex_lock(&as->map_lock);
    for (Vma *v = as->vmas; v != NULL; v = v->as_next) {
        if (index < vma_first(v) || index >= vma_end(v)) continue;
        /* Only entries that map pg: in a private mapping the entry may be the
         * process's own copy (copy on write), which is not pg's to take */
        (void)mm_zap_page_if(v->mm, v->start + ((index - v->pgoff) << PAGE_SHIFT), phys);
    }
    mutex_unlock(&as->map_lock);
}

void filemap_mkclean(AddressSpace *as, struct page *pg) {
    uint64_t index = pg->index;
    uint64_t phys = page_to_phys(pg);
    mutex_lock(&as->map_lock);
    for (Vma *v = as->vmas; v != NULL; v = v->as_next) {
        if (!(v->flags & VMA_SHARED) || index < vma_first(v) || index >= vma_end(v)) continue;
        Mm *mm = v->mm;
        uint64_t addr = v->start + ((index - v->pgoff) << PAGE_SHIFT);
        uint64_t cur;
        int changed = 0;
        spin_lock(&mm->pt_lock);
        if (vmm_lookup_in(mm->pgd, addr, &cur) && cur == phys) {
            (void)vmm_map_page_in(mm->pgd, addr, phys, mm_prot_to_vmm(v->prot & ~VMA_WRITE));   /* tables exist */
            changed = 1;
        }
        spin_unlock(&mm->pt_lock);
        if (changed) mm_flush_tlb_range(mm, addr, addr + PAGE_SIZE);
    }
    mutex_unlock(&as->map_lock);
}

int filemap_page_young(AddressSpace *as, struct page *pg) {
    uint64_t index = pg->index;
    uint64_t phys = page_to_phys(pg);
    int young = 0;
    mutex_lock(&as->map_lock);
    for (Vma *v = as->vmas; v != NULL; v = v->as_next) {
        if (index < vma_first(v) || index >= vma_end(v)) continue;
        Mm *mm = v->mm;
        uint64_t addr = v->start + ((index - v->pgoff) << PAGE_SHIFT);
        spin_lock(&mm->pt_lock);
        young += vmm_test_clear_accessed_in(mm->pgd, addr, phys);
        spin_unlock(&mm->pt_lock);
    }
    mutex_unlock(&as->map_lock);
    return young;
}

/* --- Faults --- */

/* Widen the entry at addr to the VMA's rights if it still maps expect
 * (only ever adds write: this CPU's stale entry goes, others fault again) */
static int pte_rewrite(Mm *mm, uint64_t addr, uint64_t expect, uint32_t flags) {
    int r = MM_FAULT_OK;
    uint64_t cur;
    spin_lock(&mm->pt_lock);
    if (vmm_lookup_in(mm->pgd, addr, &cur) && cur == expect && vmm_map_page_in(mm->pgd, addr, cur, flags) != 0)
        r = MM_FAULT_OOM;
    spin_unlock(&mm->pt_lock);
    mm_flush_tlb_local(addr);
    return r;
}

/* A private write: a new anonymous page with src's contents at addr, in
 * place of the entry old (replace) or where there is none (!replace). Takes
 * over nothing; src stays the caller's. */
static int cow_install(Mm *mm, Vma *v, uint64_t addr, const struct page *src, int replace, uint64_t old) {
    struct page *np = alloc_pages(0, GFP_RECLAIM);
    if (np == NULL) return MM_FAULT_OOM;
    memcpy(page_to_virt(np), page_to_virt(src), PAGE_SIZE);
    struct page *gone = NULL;
    uint64_t cur = 0;
    spin_lock(&mm->pt_lock);
    int there = vmm_lookup_in(mm->pgd, addr, &cur);
    int ok = replace ? (there && cur == old) : !there;
    if (ok && vmm_map_page_in(mm->pgd, addr, page_to_phys(np), mm_prot_to_vmm(v->prot)) != 0) ok = -1;
    spin_unlock(&mm->pt_lock);
    if (ok != 1) {
        free_pages(np, 0);
        return ok < 0 ? MM_FAULT_OOM : MM_FAULT_OK;   /* lost a race: retry from user space */
    }
    if (replace) {
        gone = pfn_to_page(old >> PAGE_SHIFT);
        mm_flush_tlb_range(mm, addr, addr + PAGE_SIZE);   /* no CPU may still read the file page there */
        filemap_page_unmapped(gone);
        mm_rss_account(mm, 1, -1);
    } else {
        mm_rss_account(mm, 1, 0);
    }
    return MM_FAULT_OK;
}

/* An entry is there: a stale TLB entry, a first store to a page mapped
 * read-only, or a private write to a cache page */
static int fault_present(Mm *mm, Vma *v, AddressSpace *as, uint64_t addr, uint64_t old, int write) {
    struct page *pg = pfn_to_page(old >> PAGE_SHIFT);
    /* Without a reference the page may be freed (a truncate) and reused at
     * any moment: only test it here; pte_rewrite rechecks the entry. M1-08
     * (COW fork): an anonymous page here may then be shared with a child, and
     * must go through the anonymous copy-on-write path instead. */
    if (!(pg->flags & PG_FILE)) return pte_rewrite(mm, addr, old, mm_prot_to_vmm(v->prot));   /* our own copy */
    if (!write) {   /* the entry allows reading already: a stale TLB entry */
        mm_flush_tlb_local(addr);
        return MM_FAULT_OK;
    }
    /* Take a reference while the entry still holds one (under the
     * page-table lock, the entry unchanged): a truncate may take the entry,
     * and with it the page, away as soon as the lock is dropped */
    uint64_t cur;
    spin_lock(&mm->pt_lock);
    int held = vmm_lookup_in(mm->pgd, addr, &cur) && cur == old;
    if (held) page_ref_get(pg);
    spin_unlock(&mm->pt_lock);
    if (!held) return MM_FAULT_OK;   /* the entry changed: fault again */
    int r = MM_FAULT_OK;
    if (!(v->flags & VMA_SHARED)) {
        r = cow_install(mm, v, addr, pg, 1, old);
    } else {
        /* Shared store: dirty and writable together under the page lock,
         * which write-back holds while it write-protects and cleans the page */
        page_lock(pg);
        if (pg->mapping == as) {
            filemap_set_dirty(pg);
            r = pte_rewrite(mm, addr, old, mm_prot_to_vmm(v->prot));
        }
        page_unlock(pg);
    }
    page_put(pg);
    return r;
}

static int file_fault(Mm *mm, Vma *v, uint64_t addr, uint32_t access) {
    File *file = v->file;
    AddressSpace *as = filemap_of(file->inode);
    uint64_t index = v->pgoff + ((addr - v->start) >> PAGE_SHIFT);
    int write = (access & MM_ACCESS_WRITE) != 0;
    int shared = (v->flags & VMA_SHARED) != 0;

    uint64_t old;
    spin_lock(&mm->pt_lock);
    int present = vmm_lookup_in(mm->pgd, addr, &old);
    spin_unlock(&mm->pt_lock);
    if (present) return fault_present(mm, v, as, addr, old, write);

    if (index >= filemap_end_index(as)) return MM_FAULT_SIGBUS;
    int seq = (v->flags & VMA_RAND_READ) ? -1 : (v->flags & VMA_SEQ_READ) ? 1 : 0;
    unsigned off;
    int err = 0;
    struct page *pg = filemap_get_page(as, index, &file->ra, seq, &off, &err);
    if (pg == NULL) return err == -ENOMEM ? MM_FAULT_OOM : MM_FAULT_SIGBUS;
    page_lock(pg);
    if (pg->mapping != as || index >= filemap_end_index(as)) {
        int gone = pg->mapping != as;
        page_unlock(pg);
        page_put(pg);
        return gone ? MM_FAULT_OK : MM_FAULT_SIGBUS;   /* truncated meanwhile: fault again, or past the new end */
    }
    if (!shared && write) {
        /* The copy goes in under the page lock: a truncate that missed it
         * unmaps again after it has dropped this page (filemap_truncate) */
        int r = cow_install(mm, v, addr, pg, 0, 0);
        page_unlock(pg);
        page_put(pg);
        return r;
    }
    if (shared && write) filemap_set_dirty(pg);
    uint32_t flags = mm_prot_to_vmm(shared && write ? v->prot : v->prot & ~VMA_WRITE);
    int r = MM_FAULT_OK;
    spin_lock(&mm->pt_lock);
    if (vmm_lookup_in(mm->pgd, addr, NULL)) {
        r = -1;   /* another thread mapped it first */
    } else if (vmm_map_page_in(mm->pgd, addr, page_to_phys(pg) + ((uint64_t)off << PAGE_SHIFT), flags) != 0) {
        r = MM_FAULT_OOM;
    } else {
        filemap_page_mapped(pg);   /* with the entry, under the lock a zap takes */
    }
    spin_unlock(&mm->pt_lock);
    page_unlock(pg);
    if (r != MM_FAULT_OK) {
        page_put(pg);
        return r < 0 ? MM_FAULT_OK : r;
    }
    mm_rss_account(mm, 0, 1);   /* the reference now belongs to the entry */
    return MM_FAULT_OK;
}

/* --- msync and madvise --- */

static int file_vma_sync(Vma *v, uint64_t start, uint64_t end) {
    uint64_t first = v->pgoff + ((start - v->start) >> PAGE_SHIFT);
    uint64_t last = first + ((end - start) >> PAGE_SHIFT) - 1;
    return filemap_write_range(vma_mapping(v), first, last);
}

static void file_vma_willneed(Vma *v, uint64_t start, uint64_t end) {
    uint64_t first = v->pgoff + ((start - v->start) >> PAGE_SHIFT);
    (void)filemap_readahead(vma_mapping(v), first, (end - start) >> PAGE_SHIFT);
}

const VmaOps filemap_vma_ops = {
    .fault = file_fault,
    .open = file_vma_open,
    .close = file_vma_close,
    .sync = file_vma_sync,
    .willneed = file_vma_willneed,
};
