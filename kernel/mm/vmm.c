/* arc_os — x86_64 page tables: the kernel's, and the walkers mm.c uses on
 * user address spaces (vmm.h).
 *
 * Owns: the kernel PML4 and HHDM mapping (vmm_init), kernel page mappings,
 * and the user page-table walkers (map, look up, unmap a range, protect,
 * copy for fork, move for mremap, free a whole address space).
 * Invariants: the kernel half (PML4 entries 256-511) of every user PML4 is a
 * copy of the kernel's; user leaf entries are 4 KiB pages, present or
 * PROT_NONE (PTE_PROTNONE, not present, frame kept); intermediate user tables
 * are freed once an unmap leaves them empty.
 * Locks: vmm_lock (LOCK_LEVEL_PAGE_TABLE) for kernel mappings. User tables
 * are changed under their mm's page-table lock, or before the mm is visible
 * to anyone (exec, fork); see mm.c.
 * Cost: one walk per page mapped; range walks skip a missing table's whole
 * span; an unmap checks each page table it touched for emptiness (512
 * entries). */

#include "mm/vmm.h"
#include "mm/pmm.h"
#include "proc/spinlock.h"
#include "arch/hal.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* SMP-safe: protects kernel page table modifications */
static Spinlock vmm_lock = SPINLOCK_INIT(LOCK_LEVEL_PAGE_TABLE, "kernel page tables");

#include "uapi/errno.h"

/* Huge page sizes and masks */
#define PAGE_SIZE_2MB       0x200000ULL
#define PAGE_MASK_2MB       (PAGE_SIZE_2MB - 1)           /* 0x1FFFFF */
#define PAGE_SIZE_1GB       0x40000000ULL
#define PAGE_MASK_1GB       (PAGE_SIZE_1GB - 1)           /* 0x3FFFFFFF */
#define PTE_ADDR_MASK_2MB   0x000FFFFFFFE00000ULL
#define PAGE_OFFSET_MASK    (PAGE_SIZE - 1)               /* 0xFFF */

/* PML4 index where kernel mappings begin (upper half) */
#define PML4_KERNEL_START   256

/* Linker symbols */
extern char _kernel_start[];
extern char _kernel_end[];

static uint64_t kernel_pml4_phys;
static uint64_t hhdm_offset;

/* Convert physical to virtual via HHDM */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

/* Physical address of a table reached through the HHDM */
static inline uint64_t vmm_hhdm_to_phys(const void *virt) {
    return (uint64_t)(uintptr_t)virt - hhdm_offset;
}

/* Allocate a zeroed page for page table use */
static uint64_t alloc_table_page(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        kprintf("[VMM] FATAL: out of memory for page table\n");
        KERNEL_PANIC();
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* Convert portable VMM flags to x86_64 PTE flags */
static uint64_t vmm_flags_to_pte(uint32_t flags) {
    uint64_t pte = PTE_PRESENT;
    if (flags & VMM_FLAG_WRITABLE) pte |= PTE_WRITABLE;
    if (flags & VMM_FLAG_USER)     pte |= PTE_USER;
    if (flags & VMM_FLAG_NOEXEC)   pte |= PTE_NX;
    if (flags & VMM_FLAG_UNCACHED) pte |= PTE_PCD | PTE_PWT;   /* PAT entry 3: UC */
    return pte;
}

/* Convert x86_64 PTE flags back to portable VMM flags (inverse of vmm_flags_to_pte) */
static uint32_t pte_to_vmm_flags(uint64_t pte_flags) {
    uint32_t flags = 0;
    if (pte_flags & PTE_WRITABLE) flags |= VMM_FLAG_WRITABLE;
    if (pte_flags & PTE_USER)     flags |= VMM_FLAG_USER;
    if (pte_flags & PTE_NX)       flags |= VMM_FLAG_NOEXEC;
    if (pte_flags & PTE_PCD) flags |= VMM_FLAG_UNCACHED;
    return flags;
}

/* Ensure a page table entry at table[index] points to a valid next-level table.
 * Returns virtual pointer to the next-level table entries. */
static uint64_t *ensure_table(uint64_t *table, uint64_t index) {
    if (!(table[index] & PTE_PRESENT)) {
        uint64_t new_table = alloc_table_page();
        /* Intermediate tables get Present + Writable + User (most permissive;
         * leaf entries apply the actual restrictions) */
        table[index] = new_table | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    return (uint64_t *)phys_to_virt(table[index] & PTE_ADDR_MASK);
}

/* Walk a 4-level page table to find the PT entry for a virtual address.
 * Returns pointer to the PT entry, or NULL if any intermediate level is absent. */
static uint64_t *walk_to_pt_entry(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) return NULL;

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) return NULL;

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT)) return NULL;

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(virt)] & PTE_ADDR_MASK);
    return &pt[PT_INDEX(virt)];
}

/* Kernel mappings: tables come from ensure_table, which panics when memory
 * runs out (the kernel's own page tables can't fail softly) */
static void kernel_map_page(uint64_t virt, uint64_t phys, uint32_t flags) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    uint64_t *pdpt = ensure_table(pml4, PML4_INDEX(virt));
    uint64_t *pd = ensure_table(pdpt, PDPT_INDEX(virt));
    uint64_t *pt = ensure_table(pd, PD_INDEX(virt));
    pt[PT_INDEX(virt)] = phys | vmm_flags_to_pte(flags);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint32_t flags) {
    uint64_t irqf;
    spin_lock_irqsave(&vmm_lock, &irqf);
    kernel_map_page(virt, phys, flags);
    spin_unlock_irqrestore(&vmm_lock, irqf);
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t irqf;
    spin_lock_irqsave(&vmm_lock, &irqf);
    vmm_unmap_page_in(kernel_pml4_phys, virt);
    spin_unlock_irqrestore(&vmm_lock, irqf);
    /* Kernel mappings are in every address space: every CPU flushes (M1-07) */
    hal_tlb_flush_kernel(virt, virt + PAGE_SIZE);
}

uint64_t vmm_get_phys(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) return 0;

    /* Check for 1GB huge page */
    if (pdpt[PDPT_INDEX(virt)] & PTE_HUGE) {
        return (pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK) + (virt & PAGE_MASK_1GB);
    }

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT)) return 0;

    /* Check for 2MB huge page */
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        return (pd[PD_INDEX(virt)] & PTE_ADDR_MASK_2MB) + (virt & PAGE_MASK_2MB);
    }

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pt[PT_INDEX(virt)] & PTE_PRESENT)) return 0;

    return (pt[PT_INDEX(virt)] & PTE_ADDR_MASK) + (virt & PAGE_OFFSET_MASK);
}

uint64_t vmm_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}

uint64_t vmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

/* Map a range using 2MB huge pages where possible, 4K pages otherwise */
static void map_range_2mb(uint64_t virt_start, uint64_t phys_start,
                          uint64_t size, uint32_t flags) {
    uint64_t pte_flags = vmm_flags_to_pte(flags);
    uint64_t offset = 0;

    while (offset < size) {
        uint64_t virt = virt_start + offset;
        uint64_t phys = phys_start + offset;
        uint64_t remaining = size - offset;

        /* Try 2MB page if aligned and enough remaining */
        if ((virt & PAGE_MASK_2MB) == 0 && (phys & PAGE_MASK_2MB) == 0 && remaining >= PAGE_SIZE_2MB) {
            uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
            uint64_t *pdpt = ensure_table(pml4, PML4_INDEX(virt));
            uint64_t *pd   = ensure_table(pdpt, PDPT_INDEX(virt));

            /* Set as 2MB huge page */
            pd[PD_INDEX(virt)] = phys | pte_flags | PTE_HUGE;
            offset += PAGE_SIZE_2MB;
        } else {
            /* Fall back to 4K page */
            vmm_map_page(virt, phys, flags);
            offset += PAGE_SIZE;
        }
    }
}

/* --- Per-process address space functions ---
 *
 * User page tables (M1-07): the mm layer (mm.c) owns them and serializes
 * changes with its page-table lock; these functions only walk and edit. A
 * failed table allocation returns -ENOMEM instead of panicking (user memory
 * may run out; the kernel's own tables may not). None of them flushes the TLB:
 * the caller does one range flush (mm.c, flush_tlb_range) and only then frees
 * what an unmap handed back, so no CPU can still reach a freed page. */

/* Page-table pages in user address spaces (/proc/meminfo PageTables) */
static uint64_t user_table_pages;

uint64_t vmm_user_table_pages(void) {
    return __atomic_load_n(&user_table_pages, __ATOMIC_RELAXED);
}

/* A zeroed user page-table page, or 0 when out of memory. */
static uint64_t alloc_user_table(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) return 0;
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    __atomic_add_fetch(&user_table_pages, 1, __ATOMIC_RELAXED);
    return phys;
}

static void free_user_table(uint64_t phys) {
    __atomic_sub_fetch(&user_table_pages, 1, __ATOMIC_RELAXED);
    pmm_free_page(phys);
}

/* Like ensure_table for user tables: NULL when out of memory. */
static uint64_t *ensure_user_table(uint64_t *table, uint64_t index) {
    if (!(table[index] & PTE_PRESENT)) {
        uint64_t new_table = alloc_user_table();
        if (new_table == 0) return NULL;
        table[index] = new_table | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    return (uint64_t *)phys_to_virt(table[index] & PTE_ADDR_MASK);
}

/* A leaf entry that holds a page: present, or PROT_NONE (kept, no access) */
static inline int pte_mapped(uint64_t pte) {
    return (pte & (PTE_PRESENT | PTE_PROTNONE)) != 0;
}

/* Leaf entry for a page with VMM_FLAG_* flags. VMM_FLAG_PROTNONE keeps the
 * frame in a not-present entry, so mprotect can give it back unchanged. */
static uint64_t user_pte(uint64_t phys, uint32_t flags) {
    if (flags & VMM_FLAG_PROTNONE) return phys | PTE_PROTNONE | PTE_USER;
    return phys | vmm_flags_to_pte(flags);
}

/* The page table holding virt's leaf entry, or NULL when a level above is
 * missing; then *skip is the first address past the missing table's span,
 * so range walks jump over unmapped space in one step. */
static uint64_t *walk_pt(uint64_t pml4_phys, uint64_t virt, uint64_t *skip) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    uint64_t e = pml4[PML4_INDEX(virt)];
    if (!(e & PTE_PRESENT)) {
        *skip = (virt | ((1ULL << 39) - 1)) + 1;
        return NULL;
    }
    uint64_t *pdpt = (uint64_t *)phys_to_virt(e & PTE_ADDR_MASK);
    e = pdpt[PDPT_INDEX(virt)];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) {
        *skip = (virt | PAGE_MASK_1GB) + 1;
        return NULL;
    }
    uint64_t *pd = (uint64_t *)phys_to_virt(e & PTE_ADDR_MASK);
    e = pd[PD_INDEX(virt)];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) {
        *skip = (virt | PAGE_MASK_2MB) + 1;
        return NULL;
    }
    return (uint64_t *)phys_to_virt(e & PTE_ADDR_MASK);
}

/* End of the page table's span that holds virt, clipped to end */
static inline uint64_t pt_span_end(uint64_t virt, uint64_t end) {
    uint64_t span = (virt | PAGE_MASK_2MB) + 1;
    return (span < end && span != 0) ? span : end;
}

static int table_empty(const uint64_t *t) {
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (t[i] != 0) return 0;
    }
    return 1;
}

uint64_t vmm_create_user_pml4(void) {
    uint64_t pml4_phys = alloc_user_table();
    if (pml4_phys == 0) return 0;
    uint64_t *new_pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    uint64_t *kern_pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);

    /* Copy kernel-half PML4 entries so kernel is mapped in every process */
    for (int i = PML4_KERNEL_START; i < PT_ENTRIES; i++) {
        new_pml4[i] = kern_pml4[i];
    }
    /* User half (0 to PML4_KERNEL_START-1) is already zeroed */

    return pml4_phys;
}

int vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint32_t flags) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    uint64_t *pdpt = ensure_user_table(pml4, PML4_INDEX(virt));
    if (pdpt == NULL) return -ENOMEM;
    uint64_t *pd = ensure_user_table(pdpt, PDPT_INDEX(virt));
    if (pd == NULL) return -ENOMEM;
    uint64_t *pt = ensure_user_table(pd, PD_INDEX(virt));
    if (pt == NULL) return -ENOMEM;

    pt[PT_INDEX(virt)] = user_pte(phys, flags);
    return 0;
}

void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = walk_to_pt_entry(pml4_phys, virt);
    if (pte == NULL) return;
    *pte = 0;
    paging_invlpg(virt);
}

uint64_t vmm_get_phys_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = walk_to_pt_entry(pml4_phys, virt);
    if (pte == NULL || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & PTE_ADDR_MASK) + (virt & PAGE_OFFSET_MASK);
}

int vmm_get_flags_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = walk_to_pt_entry(pml4_phys, virt);
    if (pte == NULL || !pte_mapped(*pte)) return -1;
    if (*pte & PTE_PROTNONE) return VMM_FLAG_PROTNONE;
    return (int)pte_to_vmm_flags(*pte);
}

int vmm_test_clear_accessed_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys) {
    uint64_t *pte = walk_to_pt_entry(pml4_phys, virt);
    if (pte == NULL || !(*pte & PTE_PRESENT) || (*pte & PTE_ADDR_MASK) != phys) return 0;
    /* Atomic: the CPU sets the bit with a locked update of its own */
    return (__atomic_fetch_and(pte, ~PTE_ACCESSED, __ATOMIC_RELAXED) & PTE_ACCESSED) != 0;
}

int vmm_lookup_in(uint64_t pml4_phys, uint64_t virt, uint64_t *phys) {
    uint64_t *pte = walk_to_pt_entry(pml4_phys, virt);
    if (pte == NULL || !pte_mapped(*pte)) return 0;
    if (phys != NULL) *phys = *pte & PTE_ADDR_MASK;
    return 1;
}

/* Calls fn for each page table under [start, end) with the part of the
 * range it covers; a missing table's whole span is skipped in one step. A
 * non-zero return from fn ends the walk and is returned. */
typedef int (*pt_visit_t)(uint64_t *pt, uint64_t from, uint64_t to, void *ctx);

static int walk_range(uint64_t pml4_phys, uint64_t start, uint64_t end, pt_visit_t fn, void *ctx) {
    uint64_t virt = start;
    while (virt < end) {
        uint64_t skip;
        uint64_t *pt = walk_pt(pml4_phys, virt, &skip);
        if (pt == NULL) {
            if (skip <= virt) break;   /* wrapped past the top */
            virt = skip;
            continue;
        }
        uint64_t span_end = pt_span_end(virt, end);
        int r = fn(pt, virt, span_end, ctx);
        if (r != 0) return r;
        if (span_end <= virt) break;
        virt = span_end;
    }
    return 0;
}

typedef struct UnmapWalk {
    uint64_t pml4;
    vmm_defer_free_t defer;
    void *ctx;
    uint64_t pages;
    uint64_t stopped;   /* where the walk stopped early (defer asked), else 0 */
} UnmapWalk;

static int unmap_visit(uint64_t *pt, uint64_t from, uint64_t to, void *ctx) {
    UnmapWalk *w = ctx;
    for (uint64_t a = from; a < to; a += PAGE_SIZE) {
        uint64_t pte = pt[PT_INDEX(a)];
        if (!pte_mapped(pte)) continue;
        pt[PT_INDEX(a)] = 0;
        w->pages++;
        if (w->defer(pte & PTE_ADDR_MASK, 0, w->ctx) != 0) {
            w->stopped = a + PAGE_SIZE;
            break;
        }
    }
    if (!table_empty(pt)) return w->stopped != 0;
    /* Free the tables this left empty, bottom up */
    uint64_t *pml4 = (uint64_t *)phys_to_virt(w->pml4);
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(from)] & PTE_ADDR_MASK);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(from)] & PTE_ADDR_MASK);
    pd[PD_INDEX(from)] = 0;
    w->defer(vmm_hhdm_to_phys(pt), 1, w->ctx);
    if (!table_empty(pd)) return w->stopped != 0;
    pdpt[PDPT_INDEX(from)] = 0;
    w->defer(vmm_hhdm_to_phys(pd), 1, w->ctx);
    if (!table_empty(pdpt)) return w->stopped != 0;
    pml4[PML4_INDEX(from)] = 0;
    w->defer(vmm_hhdm_to_phys(pdpt), 1, w->ctx);
    return w->stopped != 0;
}

uint64_t vmm_unmap_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end, vmm_defer_free_t defer, void *ctx,
                            uint64_t *next) {
    UnmapWalk w = {pml4_phys, defer, ctx, 0, 0};
    (void)walk_range(pml4_phys, start, end, unmap_visit, &w);
    if (next != NULL) *next = w.stopped != 0 && w.stopped < end ? w.stopped : end;
    return w.pages;
}

void vmm_release_table(uint64_t phys) {
    free_user_table(phys);
}

static int protect_visit(uint64_t *pt, uint64_t from, uint64_t to, void *ctx) {
    uint32_t flags = *(const uint32_t *)ctx;
    for (uint64_t a = from; a < to; a += PAGE_SIZE) {
        uint64_t pte = pt[PT_INDEX(a)];
        if (pte_mapped(pte)) pt[PT_INDEX(a)] = user_pte(pte & PTE_ADDR_MASK, flags);
    }
    return 0;
}

void vmm_protect_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end, uint32_t flags) {
    (void)walk_range(pml4_phys, start, end, protect_visit, &flags);
}

typedef struct CopyWalk {
    uint64_t dst;
    int (*skip)(uint64_t phys);
    uint64_t *copied;
} CopyWalk;

static int copy_visit(uint64_t *pt, uint64_t from, uint64_t to, void *ctx) {
    CopyWalk *w = ctx;
    for (uint64_t a = from; a < to; a += PAGE_SIZE) {
        uint64_t pte = pt[PT_INDEX(a)];
        if (!pte_mapped(pte)) continue;
        if (w->skip != NULL && w->skip(pte & PTE_ADDR_MASK)) continue;   /* the child faults it in */
        uint64_t dst_phys = pmm_alloc_page();
        if (dst_phys == 0) return -ENOMEM;
        memcpy(phys_to_virt(dst_phys), phys_to_virt(pte & PTE_ADDR_MASK), PAGE_SIZE);
        uint32_t flags = (pte & PTE_PROTNONE) ? VMM_FLAG_PROTNONE : pte_to_vmm_flags(pte);
        if (vmm_map_page_in(w->dst, a, dst_phys, flags) != 0) {
            pmm_free_page(dst_phys);
            return -ENOMEM;
        }
        (*w->copied)++;
    }
    return 0;
}

int vmm_copy_range_in(uint64_t src_pml4, uint64_t dst_pml4, uint64_t start, uint64_t end, int (*skip)(uint64_t phys),
                      uint64_t *copied) {
    CopyWalk w = {dst_pml4, skip, copied};
    return walk_range(src_pml4, start, end, copy_visit, &w);
}

int vmm_move_range_in(uint64_t pml4_phys, uint64_t from, uint64_t to, uint64_t len) {
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t *src = walk_to_pt_entry(pml4_phys, from + off);
        if (src == NULL || !pte_mapped(*src)) continue;
        uint64_t pte = *src;
        /* Tables for the destination first: on failure the source is intact */
        uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
        uint64_t *pdpt = ensure_user_table(pml4, PML4_INDEX(to + off));
        uint64_t *pd = pdpt != NULL ? ensure_user_table(pdpt, PDPT_INDEX(to + off)) : NULL;
        uint64_t *pt = pd != NULL ? ensure_user_table(pd, PD_INDEX(to + off)) : NULL;
        if (pt == NULL) return -ENOMEM;
        /* The table walk above may have added tables, never moved one */
        src = walk_to_pt_entry(pml4_phys, from + off);
        *src = 0;
        pt[PT_INDEX(to + off)] = pte;
    }
    return 0;
}

/* Free all present leaf pages in a page table, then free the PT page itself. */
static void free_pt_and_leaves(uint64_t pt_phys) {
    uint64_t *pt = (uint64_t *)phys_to_virt(pt_phys);
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (pte_mapped(pt[i])) {
            pmm_free_page(pt[i] & PTE_ADDR_MASK);
        }
    }
    free_user_table(pt_phys);
}

/* Free all page tables under a page directory, their leaves, then the PD page. */
static void free_pd_and_children(uint64_t pd_phys) {
    uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_HUGE)) continue;
        free_pt_and_leaves(pd[i] & PTE_ADDR_MASK);
    }
    free_user_table(pd_phys);
}

void vmm_free_user_pages(uint64_t pml4_phys) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

    for (int pml4_idx = 0; pml4_idx < PML4_KERNEL_START; pml4_idx++) {
        if (!(pml4[pml4_idx] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);

        for (int pdpt_idx = 0; pdpt_idx < PT_ENTRIES; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PTE_PRESENT) || (pdpt[pdpt_idx] & PTE_HUGE)) continue;
            free_pd_and_children(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        }
        free_user_table(pml4[pml4_idx] & PTE_ADDR_MASK);
    }
    free_user_table(pml4_phys);
}

/* --- Initialization --- */

void vmm_init(const BootInfo *info) {
    hhdm_offset = info->hhdm_offset;

    /* Allocate a fresh PML4 */
    kernel_pml4_phys = alloc_table_page();
    kprintf("[VMM] New PML4 at phys 0x%lx\n", kernel_pml4_phys);

    /* 1. Map the HHDM: identity-map all physical memory at hhdm_offset.
     *    We need to cover all physical memory that might be referenced. */
    uint64_t highest_phys = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        uint64_t top = info->memory_map[i].base + info->memory_map[i].length;
        if (top > highest_phys) highest_phys = top;
    }
    /* Round up to 2MB boundary for clean huge page mapping */
    highest_phys = (highest_phys + PAGE_MASK_2MB) & ~PAGE_MASK_2MB;

    kprintf("[VMM] Mapping HHDM: 0x%lx -> phys 0x0 (%lu MB)\n",
            hhdm_offset, highest_phys / (1024 * 1024));
    map_range_2mb(hhdm_offset, 0, highest_phys, VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);

    /* 2. Map the kernel image at its virtual address.
     *    Kernel runs at KERNEL_VMA (0xFFFFFFFF80000000), loaded at kernel_phys_base. */
    uint64_t kernel_virt = (uint64_t)_kernel_start;
    uint64_t kernel_phys = info->kernel_phys_base;
    uint64_t kernel_size = (uint64_t)_kernel_end - (uint64_t)_kernel_start;
    /* Round up to page boundary */
    kernel_size = PAGE_ALIGN_UP(kernel_size);

    kprintf("[VMM] Mapping kernel: 0x%lx -> phys 0x%lx (%lu KB)\n",
            kernel_virt, kernel_phys, kernel_size / 1024);
    /* Map kernel 4K at a time (it's small, and text/rodata/data have different perms
     * ideally, but for now we just map all writable) */
    for (uint64_t off = 0; off < kernel_size; off += PAGE_SIZE) {
        vmm_map_page(kernel_virt + off, kernel_phys + off, VMM_FLAG_WRITABLE);
    }

    /* 3. Map framebuffer if present */
    if (info->fb_present) {
        /* The framebuffer virtual address from Limine is in the HHDM, which we
         * already mapped. Find its physical address from the memory map. */
        for (uint64_t i = 0; i < info->memory_map_count; i++) {
            if (info->memory_map[i].type == MEMMAP_FRAMEBUFFER) {
                kprintf("[VMM] Framebuffer at phys 0x%lx (covered by HHDM)\n",
                        info->memory_map[i].base);
                break;
            }
        }
    }

    /* Switch to our page tables */
    kprintf("[VMM] Switching CR3...\n");
    paging_write_cr3(kernel_pml4_phys);

    kprintf("[VMM] Page tables active. Kernel running on own page tables.\n");
}
