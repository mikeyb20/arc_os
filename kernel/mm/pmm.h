#ifndef ARCHOS_MM_PMM_H
#define ARCHOS_MM_PMM_H

#include <stdint.h>
#include <stddef.h>
#include "boot/bootinfo.h"

#define PAGE_SIZE 4096

/* Initialize the PMM using the BootInfo memory map. */
void pmm_init(const BootInfo *info);

/* Allocate a single physical page. Returns physical address, or 0 on failure. */
uint64_t pmm_alloc_page(void);

/* Free a single physical page by its physical address. */
void pmm_free_page(uint64_t phys_addr);

/* Allocate 'count' contiguous physical pages. Returns base physical address, or 0. */
uint64_t pmm_alloc_contiguous(size_t count);

/* Get total number of physical pages managed by the PMM. */
uint64_t pmm_get_total_pages(void);

/* Get number of free physical pages. */
uint64_t pmm_get_free_pages(void);

/* --- Internal bitmap helpers (exposed for testing) --- */

void pmm_bitmap_set(uint64_t *bitmap, uint64_t bit);
void pmm_bitmap_clear(uint64_t *bitmap, uint64_t bit);
int  pmm_bitmap_test(const uint64_t *bitmap, uint64_t bit);

#endif /* ARCHOS_MM_PMM_H */
