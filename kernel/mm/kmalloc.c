#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "mm/vmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "proc/spinlock.h"

/* Heap starts at 0xFFFFFFFFC0000000 (kernel heap region) */
#define HEAP_START       0xFFFFFFFFC0000000ULL
#define HEAP_SIZE_MAX    KMALLOC_CAP_BYTES        /* 512 MB, shared with large allocations */
#define HEAP_MAX         (HEAP_START + HEAP_SIZE_MAX)

/* Block header magic canary */
#define BLOCK_MAGIC     0xDEADBEEFULL
#define FREED_POISON    0xCC

/* Number of pages in the initial heap allocation */
#define HEAP_INIT_PAGES 4

/* Minimum allocation alignment */
#define ALIGN_SIZE  16
#define ALIGN_MASK  (~(ALIGN_SIZE - 1))

/* Free-list block header */
typedef struct BlockHeader {
    uint64_t magic;               /* BLOCK_MAGIC when valid */
    size_t size;                  /* Usable size (excludes header) */
    int free;                     /* 1 if free, 0 if allocated */
    struct BlockHeader *next;     /* Next block in list */
    struct BlockHeader *prev;     /* Previous block in list */
} BlockHeader;

#define HEADER_SIZE  ((sizeof(BlockHeader) + ALIGN_SIZE - 1) & ALIGN_MASK)

/* One lock guards the block list and heap growth. It is IRQ-safe
 * (spinlock_acquire disables interrupts) because interrupt handlers allocate:
 * virtio-net rx -> TCP input -> kmalloc for a new connection. Lock order: this
 * lock is taken before the VMM and PMM locks (heap_grow maps pages). */
static Spinlock heap_lock = SPINLOCK_INIT;

static BlockHeader *heap_start_block;
static uint64_t heap_current_end;  /* Current end of mapped heap */
static size_t   heap_walk_max;     /* Most blocks one kmalloc walked (budget, ADR-0019) */
static uint64_t large_bytes;       /* Bytes in large (alloc_pages) allocations */

/* Bytes mapped for the first-fit heap. */
static inline uint64_t heap_mapped_bytes(void) {
    return heap_current_end - (uint64_t)(uintptr_t)heap_start_block;
}

/* True if ptr is in the first-fit heap rather than a large allocation. The
 * heap only grows, so a pointer it returned is always below the current end;
 * reading the end without the lock is safe (aligned 64-bit load). */
static inline int in_heap(const void *ptr) {
    uint64_t a = (uint64_t)(uintptr_t)ptr;
    return a >= (uint64_t)(uintptr_t)heap_start_block && a < __atomic_load_n(&heap_current_end, __ATOMIC_RELAXED);
}

/* Grow the heap by mapping more pages */
static int heap_grow(size_t min_bytes) {
    /* Round up to page granularity */
    size_t pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        if (heap_current_end >= HEAP_MAX) return -1;  /* Hit limit */
        /* One 512 MB cap covers the heap and large allocations (ADR-0016) */
        if (heap_mapped_bytes() + PAGE_SIZE + large_bytes > HEAP_SIZE_MAX) return -1;

        uint64_t phys = pmm_alloc_page();
        if (phys == 0) return -1;

        vmm_map_page(heap_current_end, phys, VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
        heap_current_end += PAGE_SIZE;
    }

    return 0;
}

/* Align a size up to ALIGN_SIZE */
static size_t align_up(size_t size) {
    return (size + ALIGN_SIZE - 1) & ALIGN_MASK;
}

void kmalloc_init(void) {
    heap_current_end = HEAP_START;
    heap_start_block = (BlockHeader *)HEAP_START;   /* heap_grow measures from here */

    /* Map initial heap pages (16 KB) */
    if (heap_grow(HEAP_INIT_PAGES * PAGE_SIZE) != 0) {
        kprintf("[HEAP] FATAL: cannot allocate initial heap pages\n");
        KERNEL_PANIC();
    }

    /* Initialize first free block spanning the entire initial heap */
    heap_start_block = (BlockHeader *)HEAP_START;
    heap_start_block->magic = BLOCK_MAGIC;
    heap_start_block->size  = (HEAP_INIT_PAGES * PAGE_SIZE) - HEADER_SIZE;
    heap_start_block->free  = 1;
    heap_start_block->next  = NULL;
    heap_start_block->prev  = NULL;

    kprintf("[HEAP] Initialized at 0x%lx (%lu KB initial)\n",
            HEAP_START, (HEAP_INIT_PAGES * PAGE_SIZE) / 1024);
}

/* Split a block if it's large enough to hold the requested size plus another block */
static void split_block(BlockHeader *block, size_t size) {
    if (block->size < size + HEADER_SIZE + ALIGN_SIZE) return;  /* Not worth splitting */
    size_t remaining = block->size - size - HEADER_SIZE;

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

/* Allocate from a free block: split, mark used, optionally zero. */
static void *block_allocate(BlockHeader *block, size_t size, uint32_t flags) {
    split_block(block, size);
    block->free = 0;
    void *ptr = (void *)((uint8_t *)block + HEADER_SIZE);
    if (flags & GFP_ZERO) {
        memset(ptr, 0, size);
    }
    return ptr;
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

/* Grow the heap and return a free block large enough for 'size' bytes. */
static BlockHeader *heap_grow_for(size_t size) {
    /* Find the last block */
    BlockHeader *block = heap_start_block;
    while (block->next != NULL) {
        block = block->next;
    }

    /* Calculate how much more we need.
     * Add HEADER_SIZE slack so split_block has room for a remainder block. */
    size_t need;
    if (block->free) {
        need = size - block->size + HEADER_SIZE;
    } else {
        need = size + 2 * HEADER_SIZE;
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

    return block;
}

/* Allocate with heap_lock held. */
static void *kmalloc_locked(size_t size, uint32_t flags) {
    if (size == 0) return NULL;

    size = align_up(size);

    /* First-fit search */
    BlockHeader *block = heap_start_block;
    size_t walked = 0;
    while (block != NULL) {
        if (block->magic != BLOCK_MAGIC) {
            kprintf("[HEAP] CORRUPTION: invalid magic at %p\n", (void *)block);
            KERNEL_PANIC();
        }
        walked++;
        if (block->free && block->size >= size) {
            if (walked > heap_walk_max) heap_walk_max = walked;
            return block_allocate(block, size, flags);
        }
        block = block->next;
    }
    if (walked > heap_walk_max) heap_walk_max = walked;

    /* No block found — grow the heap */
    block = heap_grow_for(size);
    if (block == NULL) return NULL;
    return block_allocate(block, size, flags);
}

/* Order of the smallest block holding `size` bytes. */
static unsigned large_order(size_t size) {
    unsigned order = 0;
    while (((size_t)PAGE_SIZE << order) < size) order++;
    return order;
}

/* Large allocation straight from the page allocator. The budget is reserved
 * under the heap lock; the page allocator and zeroing run without it. */
static void *kmalloc_large(size_t size, uint32_t flags) {
    unsigned order = large_order(size);
    uint64_t bytes = (uint64_t)PAGE_SIZE << order;

    spinlock_acquire(&heap_lock);
    if (heap_mapped_bytes() + large_bytes + bytes > HEAP_SIZE_MAX) {
        spinlock_release(&heap_lock);
        return NULL;
    }
    large_bytes += bytes;
    spinlock_release(&heap_lock);

    struct page *pg = alloc_pages(order, flags & GFP_ZERO);
    if (pg == NULL) {
        spinlock_acquire(&heap_lock);
        large_bytes -= bytes;
        spinlock_release(&heap_lock);
        return NULL;
    }
    pg->flags |= PG_KMALLOC;   /* we own the block: no lock needed */
    return page_to_virt(pg);
}

static void kfree_large(void *ptr) {
    struct page *pg = virt_to_page(ptr);
    if (((uint64_t)(uintptr_t)ptr & (PAGE_SIZE - 1)) != 0 || !pfn_valid(page_to_pfn(pg)) || !(pg->flags & PG_KMALLOC) ||
        pg->refcount != 1) {
        kprintf("[HEAP] CORRUPTION: kfree of bad pointer %p\n", ptr);
        KERNEL_PANIC();
        return;
    }
    unsigned order = page_order(pg);
    free_pages(pg, order);
    spinlock_acquire(&heap_lock);
    large_bytes -= (uint64_t)PAGE_SIZE << order;
    spinlock_release(&heap_lock);
}

static inline int is_large_size(size_t size) {
    return size >= KMALLOC_LARGE_MIN && size <= KMALLOC_LARGE_MAX;
}

void *kmalloc(size_t size, uint32_t flags) {
    if (is_large_size(size)) return kmalloc_large(size, flags);
    spinlock_acquire(&heap_lock);
    void *ptr = kmalloc_locked(size, flags);
    spinlock_release(&heap_lock);
    return ptr;
}

/* Free with heap_lock held. */
static void kfree_locked(void *ptr) {
    if (ptr == NULL) return;

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - HEADER_SIZE);

    if (block->magic != BLOCK_MAGIC) {
        kprintf("[HEAP] CORRUPTION: kfree invalid magic at %p (ptr=%p)\n",
                (void *)block, ptr);
        KERNEL_PANIC();
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

void kfree(void *ptr) {
    if (ptr == NULL) return;
    if (!in_heap(ptr)) {
        kfree_large(ptr);
        return;
    }
    spinlock_acquire(&heap_lock);
    kfree_locked(ptr);
    spinlock_release(&heap_lock);
}

/* Reallocate with heap_lock held. */
static void *krealloc_locked(void *ptr, size_t new_size) {
    if (ptr == NULL) return kmalloc_locked(new_size, GFP_KERNEL);
    if (new_size == 0) {
        kfree_locked(ptr);
        return NULL;
    }

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - HEADER_SIZE);
    if (block->magic != BLOCK_MAGIC) {
        kprintf("[HEAP] CORRUPTION: krealloc invalid magic at %p\n", (void *)block);
        KERNEL_PANIC();
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
    void *new_ptr = kmalloc_locked(new_size, GFP_KERNEL);
    if (new_ptr == NULL) return NULL;

    memcpy(new_ptr, ptr, block->size);
    kfree_locked(ptr);
    return new_ptr;
}

/* krealloc where the old or new block is a large allocation: allocate, copy,
 * free, without holding the heap lock across the calls. */
static void *krealloc_slow(void *ptr, size_t new_size) {
    size_t old_size;
    if (in_heap(ptr)) {
        old_size = ((BlockHeader *)((uint8_t *)ptr - HEADER_SIZE))->size;
    } else {
        old_size = (size_t)PAGE_SIZE << page_order(virt_to_page(ptr));
        if (is_large_size(new_size) && new_size <= old_size) return ptr;
    }
    void *new_ptr = kmalloc(new_size, GFP_KERNEL);
    if (new_ptr == NULL) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    kfree(ptr);
    return new_ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL) return kmalloc(new_size, GFP_KERNEL);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    if (!in_heap(ptr) || is_large_size(new_size)) {
        return krealloc_slow(ptr, new_size);
    }
    spinlock_acquire(&heap_lock);
    void *new_ptr = krealloc_locked(ptr, new_size);
    spinlock_release(&heap_lock);
    return new_ptr;
}

void kmalloc_dump_stats(void) {
    spinlock_acquire(&heap_lock);
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
    uint64_t mapped_end = heap_current_end;
    spinlock_release(&heap_lock);

    kprintf("[HEAP] Stats: %lu blocks (%lu free), %lu bytes used, "
            "%lu bytes free (largest=%lu)\n",
            (uint64_t)total_blocks, (uint64_t)free_blocks,
            (uint64_t)total_used, (uint64_t)total_free,
            (uint64_t)largest_free);
    kprintf("[HEAP] Heap range: 0x%lx - 0x%lx (%lu KB mapped, %lu KB large)\n", HEAP_START, mapped_end,
            (mapped_end - HEAP_START) / 1024, large_bytes / 1024);
}

void kmalloc_get_stats(HeapStats *out) {
    spinlock_acquire(&heap_lock);
    out->total_blocks = 0;
    out->free_blocks = 0;
    out->total_free = 0;
    out->total_used = 0;
    out->largest_free = 0;

    BlockHeader *block = heap_start_block;
    while (block != NULL) {
        out->total_blocks++;
        if (block->free) {
            out->free_blocks++;
            out->total_free += block->size;
            if (block->size > out->largest_free) out->largest_free = block->size;
        } else {
            out->total_used += block->size;
        }
        block = block->next;
    }

    out->heap_mapped = heap_mapped_bytes();
    out->large_bytes = large_bytes;
    spinlock_release(&heap_lock);
}

size_t kmalloc_max_walk(void) {
    spinlock_acquire(&heap_lock);
    size_t n = heap_walk_max;
    spinlock_release(&heap_lock);
    return n;
}
