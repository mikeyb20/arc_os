#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* Heap starts at 0xFFFFFFFFC0000000 (kernel heap region) */
#define HEAP_START  0xFFFFFFFFC0000000ULL
#define HEAP_MAX    0xFFFFFFFFE0000000ULL  /* 512 MB max heap */

/* Block header magic canary */
#define BLOCK_MAGIC     0xDEADBEEFULL
#define FREED_POISON    0xCC

/* Minimum allocation alignment */
#define ALIGN_SIZE  16

/* Free-list block header */
typedef struct BlockHeader {
    uint64_t magic;               /* BLOCK_MAGIC when valid */
    size_t size;                  /* Usable size (excludes header) */
    int free;                     /* 1 if free, 0 if allocated */
    struct BlockHeader *next;     /* Next block in list */
    struct BlockHeader *prev;     /* Previous block in list */
} BlockHeader;

#define HEADER_SIZE  ((sizeof(BlockHeader) + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1))

static BlockHeader *heap_start_block;
static uint64_t heap_current_end;  /* Current end of mapped heap */

/* Grow the heap by mapping more pages */
static int heap_grow(size_t min_bytes) {
    /* Round up to page granularity */
    size_t pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        if (heap_current_end >= HEAP_MAX) return -1;  /* Hit limit */

        uint64_t phys = pmm_alloc_page();
        if (phys == 0) return -1;

        vmm_map_page(heap_current_end, phys, VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
        heap_current_end += PAGE_SIZE;
    }

    return 0;
}

/* Align a size up to ALIGN_SIZE */
static size_t align_up(size_t size) {
    return (size + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
}

void kmalloc_init(void) {
    heap_current_end = HEAP_START;

    /* Map initial heap pages (16 KB) */
    if (heap_grow(4 * PAGE_SIZE) != 0) {
        kprintf("[HEAP] FATAL: cannot allocate initial heap pages\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Initialize first free block spanning the entire initial heap */
    heap_start_block = (BlockHeader *)HEAP_START;
    heap_start_block->magic = BLOCK_MAGIC;
    heap_start_block->size  = (4 * PAGE_SIZE) - HEADER_SIZE;
    heap_start_block->free  = 1;
    heap_start_block->next  = NULL;
    heap_start_block->prev  = NULL;

    kprintf("[HEAP] Initialized at 0x%lx (%lu KB initial)\n",
            HEAP_START, (4 * PAGE_SIZE) / 1024);
}

/* Split a block if it's large enough to hold the requested size plus another block */
static void split_block(BlockHeader *block, size_t size) {
    size_t remaining = block->size - size - HEADER_SIZE;
    if (remaining < ALIGN_SIZE) return;  /* Not worth splitting */

    BlockHeader *new_block = (BlockHeader *)((uint8_t *)block + HEADER_SIZE + size);
    new_block->magic = BLOCK_MAGIC;
    new_block->size  = remaining;
    new_block->free  = 1;
    new_block->next  = block->next;
    new_block->prev  = block;

    if (block->next) {
        block->next->prev = new_block;
    }
    block->next = new_block;
    block->size = size;
}

/* Coalesce a block with its next neighbor if both are free */
static void coalesce_forward(BlockHeader *block) {
    while (block->next && block->next->free) {
        BlockHeader *absorbed = block->next;
        block->size += HEADER_SIZE + absorbed->size;
        block->next = absorbed->next;
        if (absorbed->next) {
            absorbed->next->prev = block;
        }
        /* Poison absorbed header */
        memset(absorbed, FREED_POISON, HEADER_SIZE);
    }
}

void *kmalloc(size_t size, uint32_t flags) {
    if (size == 0) return NULL;

    size = align_up(size);

    /* First-fit search */
    BlockHeader *block = heap_start_block;
    while (block != NULL) {
        if (block->magic != BLOCK_MAGIC) {
            kprintf("[HEAP] CORRUPTION: invalid magic at %p\n", (void *)block);
            for (;;) __asm__ volatile ("cli; hlt");
        }

        if (block->free && block->size >= size) {
            /* Found a fit */
            split_block(block, size);
            block->free = 0;

            void *ptr = (void *)((uint8_t *)block + HEADER_SIZE);
            if (flags & GFP_ZERO) {
                memset(ptr, 0, size);
            }
            return ptr;
        }
        block = block->next;
    }

    /* No block found — grow the heap */
    /* Find the last block */
    block = heap_start_block;
    while (block->next != NULL) {
        block = block->next;
    }

    /* Calculate how much more we need */
    size_t need;
    if (block->free) {
        need = size - block->size;
    } else {
        need = size + HEADER_SIZE;
    }

    /* Save the end before growing */
    uint64_t old_end = heap_current_end;
    if (heap_grow(need) != 0) {
        return NULL;  /* Out of memory */
    }
    size_t grown = heap_current_end - old_end;

    if (block->free) {
        /* Extend the last free block */
        block->size += grown;
    } else {
        /* Create a new free block at old_end */
        BlockHeader *new_block = (BlockHeader *)old_end;
        new_block->magic = BLOCK_MAGIC;
        new_block->size  = grown - HEADER_SIZE;
        new_block->free  = 1;
        new_block->next  = NULL;
        new_block->prev  = block;
        block->next = new_block;
        block = new_block;
    }

    /* Now allocate from this block */
    split_block(block, size);
    block->free = 0;

    void *ptr = (void *)((uint8_t *)block + HEADER_SIZE);
    if (flags & GFP_ZERO) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - HEADER_SIZE);

    if (block->magic != BLOCK_MAGIC) {
        kprintf("[HEAP] CORRUPTION: kfree invalid magic at %p (ptr=%p)\n",
                (void *)block, ptr);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    if (block->free) {
        kprintf("[HEAP] WARNING: double free at %p\n", ptr);
        return;
    }

    /* Poison freed memory */
    memset(ptr, FREED_POISON, block->size);
    block->free = 1;

    /* Coalesce with neighbors */
    coalesce_forward(block);
    if (block->prev && block->prev->free) {
        coalesce_forward(block->prev);
    }
}

void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL) return kmalloc(new_size, GFP_KERNEL);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - HEADER_SIZE);
    if (block->magic != BLOCK_MAGIC) {
        kprintf("[HEAP] CORRUPTION: krealloc invalid magic at %p\n", (void *)block);
        return NULL;
    }

    new_size = align_up(new_size);

    /* If current block is big enough, just return it */
    if (block->size >= new_size) {
        split_block(block, new_size);
        return ptr;
    }

    /* Try to absorb the next free block */
    if (block->next && block->next->free &&
        block->size + HEADER_SIZE + block->next->size >= new_size) {
        coalesce_forward(block);
        block->free = 0;
        split_block(block, new_size);
        return ptr;
    }

    /* Allocate new, copy, free old */
    void *new_ptr = kmalloc(new_size, GFP_KERNEL);
    if (new_ptr == NULL) return NULL;

    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

void kmalloc_dump_stats(void) {
    size_t total_blocks = 0, free_blocks = 0;
    size_t total_free = 0, total_used = 0;
    size_t largest_free = 0;

    BlockHeader *block = heap_start_block;
    while (block != NULL) {
        total_blocks++;
        if (block->free) {
            free_blocks++;
            total_free += block->size;
            if (block->size > largest_free) largest_free = block->size;
        } else {
            total_used += block->size;
        }
        block = block->next;
    }

    kprintf("[HEAP] Stats: %lu blocks (%lu free), %lu bytes used, "
            "%lu bytes free (largest=%lu)\n",
            (uint64_t)total_blocks, (uint64_t)free_blocks,
            (uint64_t)total_used, (uint64_t)total_free,
            (uint64_t)largest_free);
    kprintf("[HEAP] Heap range: 0x%lx - 0x%lx (%lu KB mapped)\n",
            HEAP_START, heap_current_end,
            (heap_current_end - HEAP_START) / 1024);
}
