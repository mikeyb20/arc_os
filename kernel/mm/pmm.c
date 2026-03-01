#include "mm/pmm.h"
#include "boot/bootinfo.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* Bitmap: bit=1 means page is allocated, bit=0 means free */
static uint64_t *bitmap;
static uint64_t bitmap_size;    /* Size in bytes */
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t highest_addr;
static uint64_t hhdm_offset;

/* --- Bitmap helpers --- */

void pmm_bitmap_set(uint64_t *bm, uint64_t bit) {
    bm[bit / 64] |= (1ULL << (bit % 64));
}

void pmm_bitmap_clear(uint64_t *bm, uint64_t bit) {
    bm[bit / 64] &= ~(1ULL << (bit % 64));
}

int pmm_bitmap_test(const uint64_t *bm, uint64_t bit) {
    return (bm[bit / 64] >> (bit % 64)) & 1;
}

/* Find first free bit in bitmap. Returns bit index, or total_pages if none. */
static uint64_t bitmap_find_first_free(void) {
    uint64_t qwords = bitmap_size / sizeof(uint64_t);
    for (uint64_t i = 0; i < qwords; i++) {
        if (bitmap[i] != ~0ULL) {
            /* There's at least one free bit in this qword */
            for (int b = 0; b < 64; b++) {
                uint64_t page = i * 64 + (uint64_t)b;
                if (page >= total_pages) return total_pages;
                if (!(bitmap[i] & (1ULL << b))) {
                    return page;
                }
            }
        }
    }
    return total_pages;
}

/* Find 'count' contiguous free pages. Returns first page index, or total_pages. */
static uint64_t bitmap_find_contiguous(size_t count) {
    uint64_t run_start = 0;
    size_t run_length = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (!pmm_bitmap_test(bitmap, i)) {
            if (run_length == 0) run_start = i;
            run_length++;
            if (run_length >= count) return run_start;
        } else {
            run_length = 0;
        }
    }
    return total_pages;
}

void pmm_init(const BootInfo *info) {
    hhdm_offset = info->hhdm_offset;

    /* Pass 1: Find the highest usable address to determine bitmap size */
    highest_addr = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const MemoryMapEntry *e = &info->memory_map[i];
        uint64_t top = e->base + e->length;
        if (top > highest_addr) {
            highest_addr = top;
        }
    }

    total_pages = highest_addr / PAGE_SIZE;
    bitmap_size = (total_pages + 63) / 64 * sizeof(uint64_t);  /* Round up to 8-byte aligned */

    /* Pass 2: Find a usable region large enough to hold the bitmap */
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const MemoryMapEntry *e = &info->memory_map[i];
        if (e->type == MEMMAP_USABLE && e->length >= bitmap_size) {
            bitmap_phys = e->base;
            /* Align to page boundary */
            if (bitmap_phys % PAGE_SIZE != 0) {
                bitmap_phys = (bitmap_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            }
            /* Verify it still fits after alignment */
            if (bitmap_phys + bitmap_size <= e->base + e->length) {
                break;
            }
        }
        bitmap_phys = 0;
    }

    if (bitmap_phys == 0) {
        kprintf("[PMM] FATAL: no usable region for bitmap (%lu bytes needed)\n",
                bitmap_size);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Map bitmap via HHDM */
    bitmap = (uint64_t *)(bitmap_phys + hhdm_offset);

    /* Mark all pages as allocated initially */
    memset(bitmap, 0xFF, bitmap_size);
    free_pages = 0;

    /* Pass 3: Free pages in usable regions */
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const MemoryMapEntry *e = &info->memory_map[i];
        if (e->type != MEMMAP_USABLE) continue;

        uint64_t start_page = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;  /* Round up */
        uint64_t end_page   = (e->base + e->length) / PAGE_SIZE;      /* Round down */

        for (uint64_t p = start_page; p < end_page; p++) {
            pmm_bitmap_clear(bitmap, p);
            free_pages++;
        }
    }

    /* Mark page 0 as reserved (null pointer guard) */
    if (total_pages > 0 && !pmm_bitmap_test(bitmap, 0)) {
        pmm_bitmap_set(bitmap, 0);
        free_pages--;
    }

    /* Mark bitmap pages themselves as allocated */
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t bitmap_start_page = bitmap_phys / PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_pages; p++) {
        uint64_t page = bitmap_start_page + p;
        if (page < total_pages && !pmm_bitmap_test(bitmap, page)) {
            pmm_bitmap_set(bitmap, page);
            free_pages--;
        }
    }

    kprintf("[PMM] Initialized: %lu total pages, %lu free (%lu MB free)\n",
            total_pages, free_pages, (free_pages * PAGE_SIZE) / (1024 * 1024));
    kprintf("[PMM] Bitmap at phys 0x%lx (%lu bytes, %lu pages)\n",
            bitmap_phys, bitmap_size, bitmap_pages);
}

uint64_t pmm_alloc_page(void) {
    if (free_pages == 0) return 0;

    uint64_t page = bitmap_find_first_free();
    if (page >= total_pages) return 0;

    pmm_bitmap_set(bitmap, page);
    free_pages--;

    return page * PAGE_SIZE;
}

void pmm_free_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page == 0 || page >= total_pages) return;  /* Don't free page 0 or out-of-range */

    if (pmm_bitmap_test(bitmap, page)) {
        pmm_bitmap_clear(bitmap, page);
        free_pages++;
    }
}

uint64_t pmm_alloc_contiguous(size_t count) {
    if (count == 0 || free_pages < count) return 0;

    uint64_t start = bitmap_find_contiguous(count);
    if (start >= total_pages) return 0;

    for (size_t i = 0; i < count; i++) {
        pmm_bitmap_set(bitmap, start + i);
        free_pages--;
    }

    return start * PAGE_SIZE;
}

uint64_t pmm_get_total_pages(void) {
    return total_pages;
}

uint64_t pmm_get_free_pages(void) {
    return free_pages;
}
