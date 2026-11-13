/* Kernel heap: kmalloc size classes over the slab allocator, and large blocks
 * straight from the buddy allocator (M1-02b). The model is in kmalloc.h.
 *
 * Owns: the eight size-class caches and the large-block counter.
 * Invariants: a large block's head page has PG_KMALLOC and refcount 1; every
 * byte a large block or slab holds is counted by kmem_charge().
 * Locks: none of its own (each slab cache has one; counters are atomic). */

#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

static struct kmem_cache size_caches[KMALLOC_NR_CLASSES];
static const char *const class_names[KMALLOC_NR_CLASSES] = {
    "kmalloc-16",  "kmalloc-32",  "kmalloc-64",   "kmalloc-128",
    "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048",
};
static uint64_t large_bytes;   /* atomic: bytes in large blocks */

/* Size class of a request of 1..KMALLOC_SLAB_MAX bytes: 0 for 16, 7 for 2048. */
static inline unsigned size_class(size_t size) {
    if (size <= KMALLOC_MIN_SIZE) return 0;
    return (unsigned)(64 - __builtin_clzll((unsigned long long)size - 1)) - 4;
}

/* Bytes kmalloc(size) hands out, or 0 if it can't. */
static inline size_t bucket_bytes(size_t size) {
    if (size == 0 || size > KMALLOC_LARGE_MAX) return 0;
    if (size <= KMALLOC_SLAB_MAX) return (size_t)KMALLOC_MIN_SIZE << size_class(size);
    return (size_t)PAGE_SIZE << page_order_for(size);
}

void kmalloc_init(void) {
    slab_init();
    for (unsigned i = 0; i < KMALLOC_NR_CLASSES; i++) {
        kmem_cache_setup(&size_caches[i], class_names[i], (size_t)KMALLOC_MIN_SIZE << i, KMALLOC_MIN_SIZE);
    }
    kprintf("[HEAP] Slab allocator up: %u size classes (16-%u B), large blocks to %u KiB\n", KMALLOC_NR_CLASSES,
            KMALLOC_SLAB_MAX, KMALLOC_LARGE_MAX / 1024);
}

/* A large block straight from the page allocator. */
static void *kmalloc_large(size_t size, uint32_t flags) {
    unsigned order = page_order_for(size);
    if (kmem_charge(order) != 0) return NULL;
    struct page *pg = alloc_pages(order, flags & GFP_ZERO);
    if (pg == NULL) {
        kmem_uncharge(order);
        return NULL;
    }
    pg->flags |= PG_KMALLOC;   /* we own the block: no lock needed */
    __atomic_add_fetch(&large_bytes, (uint64_t)PAGE_SIZE << order, __ATOMIC_RELAXED);
    return page_to_virt(pg);
}

void *kmalloc(size_t size, uint32_t flags) {
    if (size == 0) return NULL;
    if (size <= KMALLOC_SLAB_MAX) return kmem_cache_alloc(&size_caches[size_class(size)], flags & GFP_ZERO);
    if (size > KMALLOC_LARGE_MAX) return NULL;
    return kmalloc_large(size, flags);
}

/* Descriptor of a pointer kmalloc may own (any page of a slab, or a large
 * block's head), or NULL if it can't be one. */
static struct page *owner_page(const void *ptr) {
    uint64_t a = (uint64_t)(uintptr_t)ptr;
    if (a < pmm_hhdm_offset || !pfn_valid((a - pmm_hhdm_offset) >> PAGE_SHIFT)) return NULL;
    struct page *pg = virt_to_page(ptr);
    if (pg->flags & PG_SLAB) return pg;
    if ((pg->flags & PG_KMALLOC) && (a & (PAGE_SIZE - 1)) == 0 && pg->refcount == 1) return pg;
    return NULL;
}

static void bad_pointer(const char *what, const void *ptr) {
    kprintf("[HEAP] CORRUPTION: %s of a pointer no allocation owns: %p\n", what, ptr);
    KERNEL_PANIC();
}

void kfree(void *ptr) {
    if (ptr == NULL) return;
    struct page *pg = owner_page(ptr);
    if (pg == NULL) {
        bad_pointer("kfree", ptr);
        return;
    }
    if (pg->flags & PG_SLAB) {
        kmem_cache_free(pg->slab_cache, ptr);
        return;
    }
    unsigned order = page_order(pg);
    free_pages(pg, order);
    kmem_uncharge(order);
    __atomic_sub_fetch(&large_bytes, (uint64_t)PAGE_SIZE << order, __ATOMIC_RELAXED);
}

void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL) return kmalloc(new_size, GFP_KERNEL);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    struct page *pg = owner_page(ptr);
    if (pg == NULL) {
        bad_pointer("krealloc", ptr);
        return NULL;
    }
    size_t old_size = (pg->flags & PG_SLAB) ? ((struct kmem_cache *)pg->slab_cache)->object_size
                                            : (size_t)PAGE_SIZE << page_order(pg);
    if (bucket_bytes(new_size) == old_size) return ptr;

    void *new_ptr = kmalloc(new_size, GFP_KERNEL);
    if (new_ptr == NULL) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    kfree(ptr);
    return new_ptr;
}

void kmalloc_get_stats(HeapStats *out) {
    uint64_t used, held;
    slab_get_totals(&used, &held);
    out->total_used = used;
    out->total_free = held - used;
    out->heap_mapped = held;
    out->large_bytes = __atomic_load_n(&large_bytes, __ATOMIC_RELAXED);
}

void kmalloc_dump_stats(void) {
    HeapStats hs;
    kmalloc_get_stats(&hs);
    kprintf("[HEAP] Stats: slabs hold %lu KB (%lu B in use), large blocks %lu KB, cap use %lu KB of %lu MB\n",
            (uint64_t)hs.heap_mapped / 1024, (uint64_t)hs.total_used, (uint64_t)hs.large_bytes / 1024,
            kmem_charged_bytes() / 1024, (uint64_t)(KMEM_CAP_BYTES >> 20));
    slab_dump_caches();
}
