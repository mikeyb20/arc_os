#ifndef ARCHOS_MM_VMM_H
#define ARCHOS_MM_VMM_H

#include <stdint.h>
#include "boot/bootinfo.h"

/* Portable VMM flags (mapped to architecture-specific PTE flags internally).
 * Every VMM_FLAG_* lives in this block and in the assert below, one bit each:
 * the popcount of their OR equals their number only if no two share a bit. */
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)
#define VMM_FLAG_PROTNONE  (1 << 3)   /* leaf keeps its page but allows no access (PROT_NONE, M1-07) */
#define VMM_FLAG_UNCACHED  (1 << 4)   /* device memory: no caching (MMIO, M1-09) */
#define VMM_FLAGS_ALL_     (VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NOEXEC | VMM_FLAG_PROTNONE | VMM_FLAG_UNCACHED)
#define VMM_FLAG_COUNT_    5
_Static_assert(__builtin_popcount(VMM_FLAGS_ALL_) == VMM_FLAG_COUNT_, "VMM_FLAG_* bits must be distinct");

/* Initialize VMM: create kernel page tables and switch CR3. */
void vmm_init(const BootInfo *info);

/* Map a single 4K page. virt and phys must be page-aligned. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint32_t flags);

/* Unmap a single 4K page. */
void vmm_unmap_page(uint64_t virt);

/* Get physical address for a virtual address. Returns 0 if not mapped. */
uint64_t vmm_get_phys(uint64_t virt);

/* Get the kernel PML4 physical address. */
uint64_t vmm_get_kernel_pml4(void);

/* Get the HHDM base offset (phys + offset = virt). */
uint64_t vmm_get_hhdm_offset(void);

/* User-space address space constants */
#define USER_STACK_TOP 0x00007FFFFFFFE000ULL
#define USER_BASE      0x0000000000400000ULL  /* Default ELF load address */

/* Create a new PML4 for a user process (copies kernel-half entries 256-511).
 * Returns its physical address, or 0 when out of memory. */
uint64_t vmm_create_user_pml4(void);

/* Map a 4 KiB page in a user address space (any old entry is replaced; the
 * caller frees what it held). Returns 0, or -ENOMEM when a page table can't
 * be allocated. No TLB flush: the caller flushes (mm.c). */
int vmm_map_page_in(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t flags);

/* Unmap a page in a specific address space (local TLB entry flushed). */
void vmm_unmap_page_in(uint64_t pml4, uint64_t virt);

/* Get the physical address for a virtual address in a specific address space
 * (present pages only; 0 if none). */
uint64_t vmm_get_phys_in(uint64_t pml4, uint64_t virt);

/* VMM_FLAG_* of a mapped 4 KiB page in a specific address space
 * (VMM_FLAG_PROTNONE for a PROT_NONE one), or -1 if no page is mapped. */
int vmm_get_flags_in(uint64_t pml4, uint64_t virt);

/* 1 if virt has a page (present or PROT_NONE); sets *phys (may be NULL) to
 * its frame. 0 if not. */
int vmm_lookup_in(uint64_t pml4, uint64_t virt, uint64_t *phys);

/* 1 if virt maps the frame phys (present) and the CPU marked the entry
 * accessed since the last call; clears the mark. No TLB flush (reclaim's
 * page aging, M1-14). Under the mm's page-table lock. */
int vmm_test_clear_accessed_in(uint64_t pml4, uint64_t virt, uint64_t phys);

/* Receives each frame an unmap took out: a user page (table 0) or an emptied
 * page-table page (table 1). It must not free it before the TLB flush. A
 * non-zero return for a user page stops the walk after it (the caller's
 * batch is full). */
typedef int (*vmm_defer_free_t)(uint64_t phys, int table, void *ctx);

/* Clear every leaf entry in [start, end) (page aligned) and free the page
 * tables that leaves empty, handing each frame to defer. Returns the number
 * of user pages taken out; *next (if not NULL) is where the walk stopped (end,
 * or the page after the one defer stopped it at: defer returned nonzero for a
 * page). No TLB flush. */
uint64_t vmm_unmap_range_in(uint64_t pml4, uint64_t start, uint64_t end, vmm_defer_free_t defer, void *ctx,
                            uint64_t *next);

/* Free a page-table page an unmap handed to its defer callback (keeps the
 * PageTables count right). */
void vmm_release_table(uint64_t phys);

/* Give every page mapped in [start, end) the flags (VMM_FLAG_PROTNONE
 * included). Unmapped pages stay unmapped. No TLB flush. */
void vmm_protect_range_in(uint64_t pml4, uint64_t start, uint64_t end, uint32_t flags);

/* Fork: copy every page mapped in [start, end) of src into a new page at
 * the same address in dst, same flags, except the frames skip (may be NULL)
 * returns nonzero for (the caller's page-cache pages: the child faults them
 * in); adds the pages copied to *copied. Returns 0, or -ENOMEM (dst keeps
 * what was copied; the caller destroys it). */
int vmm_copy_range_in(uint64_t src, uint64_t dst, uint64_t start, uint64_t end, int (*skip)(uint64_t phys),
                      uint64_t *copied);

/* mremap: move the entries of [from, from + len) to [to, to + len) (empty)
 * without copying pages. Returns 0, or -ENOMEM with the pages not yet moved
 * still at from. No TLB flush. */
int vmm_move_range_in(uint64_t pml4, uint64_t from, uint64_t to, uint64_t len);

/* Free all user-half leaf pages AND page table structures for a PML4.
 * After this, the PML4 is destroyed (cannot be reused). */
void vmm_free_user_pages(uint64_t pml4_phys);

/* Page-table pages held by user address spaces (/proc/meminfo PageTables). */
uint64_t vmm_user_table_pages(void);

#endif /* ARCHOS_MM_VMM_H */
