#ifndef ARCHOS_MM_VMM_H
#define ARCHOS_MM_VMM_H

#include <stdint.h>
#include "boot/bootinfo.h"

/* Portable VMM flags (mapped to architecture-specific PTE flags internally).
 * Every VMM_FLAG_* lives in this block and in the assert below, one bit each.
 * Bit 3 is taken by M1-07's VMM_FLAG_PROTNONE. */
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)
#define VMM_FLAG_UNCACHED  (1 << 4)   /* device memory: no caching (MMIO, M1-09) */
#define VMM_FLAGS_ALL_     (VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NOEXEC | VMM_FLAG_UNCACHED)
_Static_assert((VMM_FLAG_WRITABLE ^ VMM_FLAG_USER ^ VMM_FLAG_NOEXEC ^ VMM_FLAG_UNCACHED) == VMM_FLAGS_ALL_,
               "VMM_FLAG_* bits must be distinct");

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
#define USER_STACK_TOP    0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES  16  /* 64 KB; ls keeps 64 dir entries (~17 KB) on its stack */
#define USER_BASE         0x0000000000400000ULL  /* Default ELF load address */
#define USER_HEAP_BASE    0x0000000010000000ULL

/* Create a new PML4 for a user process (copies kernel-half entries 256-511). */
uint64_t vmm_create_user_pml4(void);

/* Map a page in a specific address space (given PML4 physical address). */
void vmm_map_page_in(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t flags);

/* Unmap a page in a specific address space. */
void vmm_unmap_page_in(uint64_t pml4, uint64_t virt);

/* Get the physical address for a virtual address in a specific address space. */
uint64_t vmm_get_phys_in(uint64_t pml4, uint64_t virt);

/* VMM_FLAG_* of a mapped 4 KiB page in a specific address space, or -1 if
 * the page is not mapped. */
int vmm_get_flags_in(uint64_t pml4, uint64_t virt);

/* Check that user code could access virt in this address space: present and
 * user at every level (no huge pages), and writable too when write != 0.
 * Returns 1 if so. Used by the user copy helpers (user_access.h). */
int vmm_user_access_ok(uint64_t pml4, uint64_t virt, int write);

/* Fork a user address space: create new PML4, copy all user-half pages.
 * Returns new PML4 physical address, or 0 on failure. */
uint64_t vmm_fork_address_space(uint64_t src_pml4_phys);

/* Free all user-half leaf pages AND page table structures for a PML4.
 * After this, the PML4 is destroyed (cannot be reused). */
void vmm_free_user_pages(uint64_t pml4_phys);

/* Allocate and map zeroed user stack pages in the given address space, no-
 * execute unless exec is set (PT_GNU_STACK asked for an executable stack, or
 * the program has none: Linux's default). Returns 0 or a negative errno. */
int vmm_map_user_stack(uint64_t pml4_phys, int exec);

#endif /* ARCHOS_MM_VMM_H */
