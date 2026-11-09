#ifndef ARCHOS_MM_SLAB_H
#define ARCHOS_MM_SLAB_H

#include <stddef.h>
#include <stdint.h>
#include "mm/gfp.h"
#include "mm/page.h"
#include "proc/spinlock.h"

/* Slab allocator: object caches on top of the buddy allocator (M1-02b,
 * ADR-0019).
 *
 * A cache hands out objects of one size. It carves them from slabs: one buddy
 * block of order 0 (order 1-3 for objects over 512 bytes) reached through the
 * HHDM. All slab bookkeeping lives in the slab's page descriptors, so objects
 * carry no header:
 *   - every page of a slab has PG_SLAB and slab_cache = its cache, so the
 *     owner of any object is virt -> page -> cache in O(1);
 *   - the head page holds the in-use count, the free-object list and the
 *     "fresh" cursor (objects never handed out yet, so a new slab needs no
 *     set-up loop), and links the slab on one of the cache's three lists:
 *     partial, full or empty.
 * Allocation and free are O(1): no loop anywhere on those paths.
 *
 * Empty slabs above a reserve of one per cache go straight back to the buddy
 * allocator; kmem_cache_shrink() returns the reserve too (reclaim, M1-14).
 *
 * Locking: one IRQ-safe spinlock per cache (work-blocks.md §7 level 10: a slab
 * cache lock is held only around its own lists, and the buddy lock is the
 * only lock taken under it). Per-CPU object caches are later and additive.
 *
 * Debug builds (ARC_DEBUG, on by default; see kernel/CMakeLists.txt) add an
 * 8-byte red zone after each object, poison freed objects, check the poison
 * when an object is reused and catch double frees. Release builds do none of
 * this and store the free-list link in the object itself. */

/* No free object: the value of page->slab_free on a slab with an empty free list. */
#define SLAB_FREE_NONE 0xFFFFFFFFu

/* Largest object a cache can hold (an order-3 slab still holds three). */
#define SLAB_OBJECT_MAX 8192u

/* One cap for every kernel heap page: slabs plus large kmalloc blocks
 * (ADR-0016). It is an accounting limit, not an address window. */
#define KMEM_CAP_BYTES (512ull * 1024 * 1024)

/* One object cache. Static caches (the kmalloc size classes, the cache of
 * caches) are filled in by kmem_cache_setup(); the rest come from
 * kmem_cache_create(). Fields below the lock are guarded by it. */
struct kmem_cache {
    Spinlock lock;
    const char *name;
    uint32_t object_size;      /* bytes the caller may use */
    uint32_t stride;           /* bytes between objects (object, red zone, alignment) */
    uint32_t align;            /* object alignment, a power of two >= 8 */
    uint16_t order;            /* buddy order of one slab */
    uint16_t per_slab;         /* objects in one slab */
    struct page_list partial;  /* slabs with free and used objects */
    struct page_list full;     /* slabs with no free object */
    struct page_list empty;    /* slabs with no used object (normally at most one) */
    uint32_t nr_empty;         /* slabs on `empty` */
    uint64_t nr_slabs;         /* slabs held */
    uint64_t active;           /* objects handed out */
    struct kmem_cache *next;   /* registry of all caches (slab.c) */
};

/* One cache line or two: caches are few and never freed, but every alloc and
 * free touches one (ADR-0019). */
_Static_assert(sizeof(struct kmem_cache) <= 128, "struct kmem_cache must stay within two cache lines");

/* Counters for one cache, as /proc/slabinfo prints them. */
typedef struct {
    const char *name;
    uint32_t object_size;
    uint64_t active_objs;  /* objects handed out */
    uint64_t total_objs;   /* objects the cache's slabs can hold */
    uint64_t slabs;
    uint32_t objs_per_slab;
    uint32_t pages_per_slab;
} SlabInfo;

/* Set up the cache of caches. Call once, before any other slab call. */
void slab_init(void);

/* Fill in caller-owned storage as a cache and register it. Needs no memory,
 * so it works before any allocator is up (bootstrap). `align` 0 means 8.
 * Returns 0, or -1 if `size` is 0 or over SLAB_OBJECT_MAX or `align` is not a
 * power of two up to PAGE_SIZE. */
int kmem_cache_setup(struct kmem_cache *cache, const char *name, size_t size, size_t align);

/* Create a cache for objects of `size` bytes aligned to `align` (0 = 8). The
 * name is kept by pointer and must outlive the cache. Returns NULL on bad
 * arguments or no memory. Caches are never destroyed. */
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align);

/* Allocate one object. Flags: GFP_ZERO zeroes it; GFP_DMA32 is not supported
 * (DMA buffers come from alloc_pages). Returns NULL when no slab can be had.
 * O(1). */
void *kmem_cache_alloc(struct kmem_cache *cache, uint32_t flags);

/* Return an object to its cache. NULL is ignored. Debug builds panic on an
 * object of another cache, a pointer inside an object, a double free or an
 * overwritten red zone. O(1). */
void kmem_cache_free(struct kmem_cache *cache, void *obj);

/* Give every empty slab of the cache back to the buddy allocator, the reserve
 * included. Returns the number of pages freed. The hook reclaim calls (M1-14). */
uint64_t kmem_cache_shrink(struct kmem_cache *cache);

/* Owning cache of an address inside a slab, or NULL if no slab holds it.
 * The address must be in the HHDM range of a valid PFN. O(1). */
struct kmem_cache *slab_owner(const void *addr);

/* Usable bytes of a slab object (the cache's object size). */
size_t slab_object_size(const void *obj);

/* Counters of the n-th registered cache, in creation order. Returns 0, or -1
 * when there is no such cache. */
int slab_get_info(unsigned n, SlabInfo *out);

/* Sums over every cache: bytes of objects handed out (object size each) and
 * bytes of pages held by slabs. */
void slab_get_totals(uint64_t *used_bytes, uint64_t *slab_bytes);

/* Print one line per cache to the kernel log. */
void slab_dump_caches(void);

/* --- Heap accounting (ADR-0016: one 512 MB cap for slabs and large kmalloc
 * blocks) --- */

/* Count 2^order pages against the cap. Returns 0, or -1 if the cap would be
 * passed (nothing is counted then). */
int kmem_charge(unsigned order);

/* Undo kmem_charge(order). */
void kmem_uncharge(unsigned order);

/* Bytes counted against the cap (slab pages plus large kmalloc blocks). */
uint64_t kmem_charged_bytes(void);

/* Most steps one kmem_cache_alloc/kmem_cache_free has taken since boot (a
 * budget, ADR-0019): 1 for the object, +1 when its slab changes lists, +1
 * when a slab comes from or goes back to the buddy allocator. */
uint32_t slab_max_steps(void);

/* Steps of the most recent kmem_cache_alloc/kmem_cache_free. Host tests
 * only: concurrent callers overwrite it. */
uint32_t slab_last_steps(void);

#endif /* ARCHOS_MM_SLAB_H */
