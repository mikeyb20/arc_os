#ifndef ARCHOS_MM_VMM_H
#define ARCHOS_MM_VMM_H

#include <stdint.h>
#include "boot/bootinfo.h"

/* Portable VMM flags (mapped to architecture-specific PTE flags internally) */
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)

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
#define USER_STACK_PAGES  4   /* 16 KB */
#define USER_BASE         0x0000000000400000ULL  /* Default ELF load address */
#define USER_HEAP_BASE    0x0000000010000000ULL

/* Create a new PML4 for a user process (copies kernel-half entries 256-511). */
uint64_t vmm_create_user_pml4(void);

/* Destroy a user PML4 — frees user-half page table pages (not leaf pages). */
void vmm_destroy_user_pml4(uint64_t pml4);

/* Map a page in a specific address space (given PML4 physical address). */
void vmm_map_page_in(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t flags);

/* Unmap a page in a specific address space. */
void vmm_unmap_page_in(uint64_t pml4, uint64_t virt);

/* Get the physical address for a virtual address in a specific address space. */
uint64_t vmm_get_phys_in(uint64_t pml4, uint64_t virt);

#endif /* ARCHOS_MM_VMM_H */
