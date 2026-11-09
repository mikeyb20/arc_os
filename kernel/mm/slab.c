/* Slab allocator (M1-02b, ADR-0019). The model is in slab.h.
 *
 * Owns: object caches, their slabs (buddy blocks with PG_SLAB on every page),
 * the registry of caches and the heap cap (ADR-0016).
 *
 * Invariants, under the cache's lock:
 *   - A slab is on exactly one of its cache's lists: `full` when
 *     slab_inuse == per_slab, `empty` when slab_inuse == 0, `partial` otherwise.
 *   - Object i of a slab starts at byte i * stride of the slab. Objects
 *     [0, slab_fresh) have been handed out at least once; the free ones among
 *     them are on the slab_free list (byte offsets, linked through a u32 at
 *     link_off inside each free object). Objects [slab_fresh, per_slab) were
 *     never handed out and are not on the list.
 *   - nr_empty <= 1: one reserve slab per cache; a second empty slab goes
 *     straight back to the buddy allocator.
 *   - Debug: a handed-out object's red zone holds RZ_LIVE, a free one RZ_FREE
 *     and its bytes [0, rz_off) hold POISON_FREE.
 *
 * Locks: each cache's lock (IRQ-safe, §7 level 10, may take the buddy lock
 * under it); registry_lock (IRQ-safe, guards the cache list; a cache lock may
 * be taken under it, never the other way round). */

#include "mm/slab.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "proc/spinlock.h"

/* Debug checks (ARC_DEBUG from kernel/CMakeLists.txt). A constant, so release
 * builds compile the checks away; host tests define the knob as a variable to
 * run both modes. */
#ifndef SLAB_TEST_DEBUG_KNOB
#if defined(ARC_DEBUG) && ARC_DEBUG
static const int slab_debug = 1;
#else
static const int slab_debug = 0;
#endif
#endif

#define SLAB_REDZONE   8u           /* debug: u32 state + u32 free-list link after the object */
#define RZ_LIVE        0xCCCCCCCCu  /* debug red-zone state: object handed out */
#define RZ_FREE        0xBBBBBBBBu  /* debug red-zone state: object free */
#define POISON_FREE    0x6B         /* debug: fill of a freed object */
#define SLAB_MIN_ALIGN 8u
#define SLAB_BIG_OBJ   512u         /* objects over this get order 1-3 slabs */
#define SLAB_MIN_OBJS  8u           /* aim for at least this many objects per big slab */
#define SLAB_MAX_ORDER 3u

static struct kmem_cache cache_cache;   /* holds every kmem_cache_create()d cache */
static struct kmem_cache *registry_head;
static struct kmem_cache *registry_tail;
static Spinlock registry_lock = SPINLOCK_INIT;

static uint64_t charged_bytes;   /* atomic: slab pages + large kmalloc blocks */
static uint32_t max_steps;       /* atomic: budget slab_steps_max */
static uint32_t last_steps;      /* host tests only */

/* --- Heap accounting --- */

int kmem_charge(unsigned order) {
    uint64_t bytes = (uint64_t)PAGE_SIZE << order;
    if (__atomic_add_fetch(&charged_bytes, bytes, __ATOMIC_RELAXED) > KMEM_CAP_BYTES) {
        __atomic_sub_fetch(&charged_bytes, bytes, __ATOMIC_RELAXED);
        return -1;
    }
    return 0;
}

void kmem_uncharge(unsigned order) {
    __atomic_sub_fetch(&charged_bytes, (uint64_t)PAGE_SIZE << order, __ATOMIC_RELAXED);
}

uint64_t kmem_charged_bytes(void) {
    return __atomic_load_n(&charged_bytes, __ATOMIC_RELAXED);
}

/* --- Layout helpers --- */

/* Offset of the debug red zone: the object size rounded up to 8. */
static inline uint32_t rz_off(const struct kmem_cache *c) {
    return (c->object_size + 7u) & ~7u;
}

/* Where a free object keeps its free-list link: inside the red zone in debug
 * builds (so the whole object can be poisoned), at the start otherwise. */
static inline uint32_t link_off(const struct kmem_cache *c) {
    return slab_debug ? rz_off(c) + 4u : 0u;
}

static inline uint32_t *rz_state(const struct kmem_cache *c, uint8_t *obj) {
    return (uint32_t *)(obj + rz_off(c));
}

static inline uint32_t *obj_link(const struct kmem_cache *c, uint8_t *obj) {
    return (uint32_t *)(obj + link_off(c));
}

/* Head page of the slab holding `pg`. Slabs are buddy blocks, aligned to
 * their size, so the head is the PFN rounded down to the slab order. */
static inline struct page *slab_head(const struct kmem_cache *c, struct page *pg) {
    uint64_t pfn = page_to_pfn(pg) & ~(((uint64_t)1 << c->order) - 1);
    return pfn_to_page(pfn);
}

static void note_steps(uint32_t steps) {
    last_steps = steps;
    uint32_t cur = __atomic_load_n(&max_steps, __ATOMIC_RELAXED);
    while (steps > cur &&
           !__atomic_compare_exchange_n(&max_steps, &cur, steps, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* Report a bad free or a corrupted object. Callers return right after. */
static void slab_bad(const struct kmem_cache *c, const void *obj, const char *why) {
    kprintf("[SLAB] CORRUPTION: %s: object %p of cache %s\n", why, obj, c ? c->name : "?");
    KERNEL_PANIC();
}

/* --- Cache set-up and registry --- */

int kmem_cache_setup(struct kmem_cache *cache, const char *name, size_t size, size_t align) {
    if (align == 0) align = SLAB_MIN_ALIGN;
    if (size == 0 || size > SLAB_OBJECT_MAX || (align & (align - 1)) != 0 || align > PAGE_SIZE) return -1;
    if (align < SLAB_MIN_ALIGN) align = SLAB_MIN_ALIGN;

    memset(cache, 0, sizeof(*cache));
    cache->lock = (Spinlock)SPINLOCK_INIT;
    cache->name = name;
    cache->object_size = (uint32_t)size;
    cache->align = (uint32_t)align;
    uint32_t bytes = slab_debug ? rz_off(cache) + SLAB_REDZONE : (uint32_t)size;
    cache->stride = (bytes + (uint32_t)align - 1) & ~((uint32_t)align - 1);

    unsigned order = 0;
    if (size > SLAB_BIG_OBJ) {
        order = 1;
        while (order < SLAB_MAX_ORDER && ((uint32_t)PAGE_SIZE << order) / cache->stride < SLAB_MIN_OBJS) order++;
    }
    cache->order = (uint16_t)order;
    cache->per_slab = (uint16_t)(((uint32_t)PAGE_SIZE << order) / cache->stride);
    cache->partial = (struct page_list)PAGE_LIST_INIT;
    cache->full = (struct page_list)PAGE_LIST_INIT;
    cache->empty = (struct page_list)PAGE_LIST_INIT;

    spinlock_acquire(&registry_lock);
    if (registry_tail) {
        registry_tail->next = cache;
    } else {
        registry_head = cache;
    }
    registry_tail = cache;
    spinlock_release(&registry_lock);
    return 0;
}

void slab_init(void) {
    kmem_cache_setup(&cache_cache, "kmem_cache", sizeof(struct kmem_cache), 0);
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align) {
    struct kmem_cache *c = kmem_cache_alloc(&cache_cache, 0);
    if (c == NULL) return NULL;
    if (kmem_cache_setup(c, name, size, align) != 0) {
        kmem_cache_free(&cache_cache, c);
        return NULL;
    }
    return c;
}

/* --- Slabs --- */

/* Get a new slab from the buddy allocator. Cache lock held (the buddy lock is
 * a leaf under it). */
static struct page *slab_new(struct kmem_cache *c) {
    if (kmem_charge(c->order) != 0) return NULL;
    struct page *pg = alloc_pages(c->order, GFP_KERNEL);
    if (pg == NULL) {
        kmem_uncharge(c->order);
        return NULL;
    }
    pg->flags |= PG_SLAB;
    pg->slab_inuse = 0;
    pg->slab_fresh = 0;
    pg->slab_free = SLAB_FREE_NONE;
    pg->slab_cache = c;
    for (unsigned i = 1; i < (1u << c->order); i++) {   /* at most 7 tail pages */
        pg[i].flags = PG_SLAB;
        pg[i].slab_cache = c;
    }
    c->nr_slabs++;
    return pg;
}

/* Give a slab (off every list) back to the buddy allocator, leaving its
 * descriptors as alloc_pages handed them out. Cache lock held. */
static void slab_release(struct kmem_cache *c, struct page *pg) {
    for (unsigned i = 1; i < (1u << c->order); i++) {
        pg[i].flags = 0;
        pg[i].slab_cache = NULL;
    }
    pg->flags &= ~PG_SLAB;
    pg->mapcount = 0;   /* slab_inuse, slab_fresh */
    pg->index = 0;      /* slab_free */
    pg->slab_cache = NULL;
    c->nr_slabs--;
    free_pages(pg, c->order);
    kmem_uncharge(c->order);
}

/* Take one object from a slab that has one. Cache lock held. Returns NULL
 * (after a panic report) if a free object was overwritten. */
static void *slab_take(struct kmem_cache *c, struct page *pg) {
    uint8_t *base = page_to_virt(pg);
    uint8_t *obj;
    if (pg->slab_free != SLAB_FREE_NONE) {
        obj = base + pg->slab_free;
        if (slab_debug) {
            if (*rz_state(c, obj) != RZ_FREE) {
                slab_bad(c, obj, "free object's red zone overwritten");
                return NULL;
            }
            for (uint32_t i = 0; i < rz_off(c); i++) {
                if (obj[i] != POISON_FREE) {
                    slab_bad(c, obj, "object written after free");
                    return NULL;
                }
            }
        }
        pg->slab_free = *obj_link(c, obj);
    } else {
        obj = base + (uint32_t)pg->slab_fresh * c->stride;
        pg->slab_fresh++;
    }
    if (slab_debug) *rz_state(c, obj) = RZ_LIVE;
    pg->slab_inuse++;
    return obj;
}

void *kmem_cache_alloc(struct kmem_cache *cache, uint32_t flags) {
    uint32_t steps = 1;
    spinlock_acquire(&cache->lock);
    struct page *pg = page_list_first(&cache->partial);
    if (pg == NULL) {
        pg = page_list_first(&cache->empty);
        if (pg != NULL) {
            page_list_remove(&cache->empty, pg);
            cache->nr_empty--;
        } else {
            pg = slab_new(cache);
            if (pg == NULL) {
                spinlock_release(&cache->lock);
                return NULL;
            }
        }
        page_list_push(&cache->partial, pg);
        steps++;
    }
    void *obj = slab_take(cache, pg);
    if (obj == NULL) {
        spinlock_release(&cache->lock);
        return NULL;
    }
    if (pg->slab_inuse == cache->per_slab) {
        page_list_remove(&cache->partial, pg);
        page_list_push(&cache->full, pg);
        steps++;
    }
    cache->active++;
    note_steps(steps);
    spinlock_release(&cache->lock);

    if (flags & GFP_ZERO) memset(obj, 0, cache->object_size);
    return obj;
}

/* Byte offset of `obj` in the slab whose head is `head`. */
static inline uint32_t obj_offset(const struct page *head, const void *obj) {
    return (uint32_t)((uint64_t)(uintptr_t)obj - (uint64_t)(uintptr_t)page_to_virt(head));
}

void kmem_cache_free(struct kmem_cache *cache, void *obj) {
    if (obj == NULL) return;
    if (slab_debug && slab_owner(obj) != cache) {
        slab_bad(cache, obj, "free to the wrong cache or of a non-slab pointer");
        return;
    }
    struct page *pg = slab_head(cache, virt_to_page(obj));
    uint8_t *o = obj;
    uint32_t off = obj_offset(pg, o);
    if (slab_debug && (off % cache->stride != 0 || off / cache->stride >= cache->per_slab)) {
        slab_bad(cache, obj, "free of a pointer inside an object");
        return;
    }

    uint32_t steps = 1;
    spinlock_acquire(&cache->lock);
    if (slab_debug) {
        uint32_t state = *rz_state(cache, o);
        if (state != RZ_LIVE || off / cache->stride >= pg->slab_fresh) {
            spinlock_release(&cache->lock);
            slab_bad(cache, obj, state == RZ_FREE ? "double free" : "red zone overwritten (overflow?)");
            return;
        }
        memset(o, POISON_FREE, rz_off(cache));
        *rz_state(cache, o) = RZ_FREE;
    }
    *obj_link(cache, o) = pg->slab_free;
    pg->slab_free = off;

    int was_full = pg->slab_inuse == cache->per_slab;
    pg->slab_inuse--;
    cache->active--;
    if (pg->slab_inuse == 0) {
        page_list_remove(was_full ? &cache->full : &cache->partial, pg);
        steps++;
        if (cache->nr_empty >= 1) {   /* keep one empty slab as a reserve */
            slab_release(cache, pg);
            steps++;
        } else {
            page_list_push(&cache->empty, pg);
            cache->nr_empty++;
        }
    } else if (was_full) {
        page_list_remove(&cache->full, pg);
        page_list_push(&cache->partial, pg);
        steps++;
    }
    note_steps(steps);
    spinlock_release(&cache->lock);
}

uint64_t kmem_cache_shrink(struct kmem_cache *cache) {
    uint64_t pages = 0;
    spinlock_acquire(&cache->lock);
    struct page *pg;
    while ((pg = page_list_first(&cache->empty)) != NULL) {   /* normally one slab */
        page_list_remove(&cache->empty, pg);
        cache->nr_empty--;
        slab_release(cache, pg);
        pages += 1u << cache->order;
    }
    spinlock_release(&cache->lock);
    return pages;
}

/* --- Lookups and statistics --- */

struct kmem_cache *slab_owner(const void *addr) {
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if (a < pmm_hhdm_offset) return NULL;
    uint64_t pfn = (a - pmm_hhdm_offset) >> PAGE_SHIFT;
    if (!pfn_valid(pfn)) return NULL;
    struct page *pg = pfn_to_page(pfn);
    return (pg->flags & PG_SLAB) ? pg->slab_cache : NULL;
}

size_t slab_object_size(const void *obj) {
    struct kmem_cache *c = slab_owner(obj);
    return c ? c->object_size : 0;
}

int slab_get_info(unsigned n, SlabInfo *out) {
    spinlock_acquire(&registry_lock);
    struct kmem_cache *c = registry_head;
    while (c != NULL && n > 0) {
        c = c->next;
        n--;
    }
    if (c == NULL) {
        spinlock_release(&registry_lock);
        return -1;
    }
    spinlock_acquire(&c->lock);
    out->name = c->name;
    out->object_size = c->object_size;
    out->active_objs = c->active;
    out->objs_per_slab = c->per_slab;
    out->total_objs = c->nr_slabs * c->per_slab;
    out->slabs = c->nr_slabs;
    out->pages_per_slab = 1u << c->order;
    spinlock_release(&c->lock);
    spinlock_release(&registry_lock);
    return 0;
}

void slab_get_totals(uint64_t *used_bytes, uint64_t *slab_bytes) {
    uint64_t used = 0, held = 0;
    spinlock_acquire(&registry_lock);
    for (struct kmem_cache *c = registry_head; c != NULL; c = c->next) {
        spinlock_acquire(&c->lock);
        used += c->active * c->object_size;
        held += c->nr_slabs * ((uint64_t)PAGE_SIZE << c->order);
        spinlock_release(&c->lock);
    }
    spinlock_release(&registry_lock);
    *used_bytes = used;
    *slab_bytes = held;
}

void slab_dump_caches(void) {
    SlabInfo si;
    kprintf("[SLAB] caches%s: name objsize per-slab pages active/total\n", slab_debug ? " (debug checks on)" : "");
    for (unsigned n = 0; slab_get_info(n, &si) == 0; n++) {
        kprintf("[SLAB]   %s %u %u %u %lu/%lu\n", si.name, si.object_size, si.objs_per_slab, si.pages_per_slab,
                si.active_objs, si.total_objs);
    }
}

uint32_t slab_max_steps(void) {
    return __atomic_load_n(&max_steps, __ATOMIC_RELAXED);
}

uint32_t slab_last_steps(void) {
    return last_steps;
}
