/* arc_os libc — malloc/free via sbrk (SYS_BRK)
 * Simple free-list allocator, similar to kernel's kmalloc. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#define ALIGN_SIZE  16
#define ALIGN_MASK  (~(size_t)(ALIGN_SIZE - 1))
#define BLOCK_MAGIC 0xA110CA7EULL

typedef struct BlockHeader {
    uint64_t magic;
    size_t   size;
    int      free;
    struct BlockHeader *next;
    struct BlockHeader *prev;
} BlockHeader;

#define HEADER_SIZE ((sizeof(BlockHeader) + ALIGN_SIZE - 1) & ALIGN_MASK)

static BlockHeader *heap_start;
static int heap_initialized;

static size_t align_up(size_t size) {
    return (size + ALIGN_SIZE - 1) & ALIGN_MASK;
}

static BlockHeader *extend_heap(size_t size) {
    size_t total = HEADER_SIZE + size;
    /* Round up to 4096 for fewer sbrk calls */
    total = (total + 4095) & ~(size_t)4095;
    void *p = sbrk((intptr_t)total);
    if (p == (void *)-1) return NULL;

    BlockHeader *block = (BlockHeader *)p;
    block->magic = BLOCK_MAGIC;
    block->size = total - HEADER_SIZE;
    block->free = 1;
    block->next = NULL;
    block->prev = NULL;
    return block;
}

static void split_block(BlockHeader *block, size_t size) {
    if (block->size < size + HEADER_SIZE + ALIGN_SIZE) return;
    size_t remaining = block->size - size - HEADER_SIZE;

    BlockHeader *new_block = (BlockHeader *)((uint8_t *)block + HEADER_SIZE + size);
    new_block->magic = BLOCK_MAGIC;
    new_block->size = remaining;
    new_block->free = 1;
    new_block->next = block->next;
    new_block->prev = block;
    if (block->next) block->next->prev = new_block;
    block->next = new_block;
    block->size = size;
}

static void coalesce(BlockHeader *block) {
    while (block->next && block->next->free) {
        BlockHeader *absorbed = block->next;
        block->size += HEADER_SIZE + absorbed->size;
        block->next = absorbed->next;
        if (absorbed->next) absorbed->next->prev = block;
    }
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size);

    if (!heap_initialized) {
        heap_start = extend_heap(size);
        if (!heap_start) return NULL;
        heap_initialized = 1;
    }

    /* First-fit search */
    BlockHeader *block = heap_start;
    while (block) {
        if (block->free && block->size >= size) {
            split_block(block, size);
            block->free = 0;
            return (void *)((uint8_t *)block + HEADER_SIZE);
        }
        if (!block->next) break;
        block = block->next;
    }

    /* Extend heap */
    BlockHeader *new_block = extend_heap(size);
    if (!new_block) return NULL;
    if (block) {
        block->next = new_block;
        new_block->prev = block;
    }
    split_block(new_block, size);
    new_block->free = 0;
    return (void *)((uint8_t *)new_block + HEADER_SIZE);
}

void free(void *ptr) {
    if (!ptr) return;
    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - HEADER_SIZE);
    if (block->magic != BLOCK_MAGIC) return;
    block->free = 1;
    coalesce(block);
    if (block->prev && block->prev->free) {
        coalesce(block->prev);
    }
}

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return NULL; }

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - HEADER_SIZE);
    if (block->magic != BLOCK_MAGIC) return NULL;

    new_size = align_up(new_size);
    if (block->size >= new_size) return ptr;

    void *new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}
