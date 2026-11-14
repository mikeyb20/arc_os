#ifndef ARCHOS_MM_VMA_H
#define ARCHOS_MM_VMA_H

/* Virtual memory areas (M1-07, ADR-0004).
 *
 * A VMA is one run of user addresses [start, end) with the same protection
 * and kind. An address space (struct Mm, mm.h) keeps its VMAs in a VmaSet: a
 * red-black tree keyed by start address for O(log n) lookup, plus a sorted
 * doubly linked list for neighbours and in-order walks. VMAs never overlap;
 * both ends are page aligned.
 *
 * This file is only the data structure: no page tables, no locking. The
 * owner (mm.c) holds the mm lock for writing around every change and for
 * reading around lookups. */

#include <stdint.h>
#include "lib/rbtree.h"

/* Protection, Linux PROT_* values */
#define VMA_READ      0x1u
#define VMA_WRITE     0x2u
#define VMA_EXEC      0x4u
#define VMA_PROT_MASK (VMA_READ | VMA_WRITE | VMA_EXEC)

/* Kind and hints */
#define VMA_ANON       (1u << 0)   /* anonymous: zero-filled on first touch */
#define VMA_SHARED     (1u << 1)   /* MAP_SHARED (file mappings, M1-12) */
#define VMA_GROWSDOWN  (1u << 2)   /* the user stack: a fault just below grows it */
#define VMA_HEAP       (1u << 3)   /* brk heap ("[heap]"); merges only with itself */
#define VMA_STACK      (1u << 4)   /* the main thread's stack ("[stack]") */
#define VMA_HUGEPAGE   (1u << 5)   /* madvise hints, kept for M1-08 */
#define VMA_NOHUGEPAGE (1u << 6)
#define VMA_SEQ_READ   (1u << 7)   /* MADV_SEQUENTIAL (readahead, M1-12) */
#define VMA_RAND_READ  (1u << 8)   /* MADV_RANDOM */

struct Vma;

/* Per-kind fault operation. Anonymous VMAs have none (mm.c maps a zeroed
 * page); file VMAs get one from M1-12. Returns 0 with *phys set to the page
 * to map, or a negative errno. */
typedef struct VmaOps {
    int (*fault)(struct Vma *vma, uint64_t addr, uint32_t access, uint64_t *phys);
} VmaOps;

typedef struct Vma {
    RbNode rb;              /* in VmaSet.tree, keyed by start */
    struct Vma *prev;       /* sorted list: lower neighbour, or NULL */
    struct Vma *next;       /* higher neighbour, or NULL */
    uint64_t start;         /* first byte, page aligned */
    uint64_t end;           /* one past the last byte, page aligned */
    uint32_t prot;          /* VMA_READ | VMA_WRITE | VMA_EXEC */
    uint32_t flags;         /* VMA_ANON ... */
    void *file;             /* backing file (M1-12); NULL when anonymous */
    uint64_t pgoff;         /* file offset of start, in pages (M1-12) */
    const VmaOps *ops;      /* NULL for anonymous memory */
} Vma;

/* Allocated from its own slab cache: keep it small (ADR-0019) */
_Static_assert(sizeof(Vma) <= 96, "struct Vma must stay within 96 bytes");

typedef struct VmaSet {
    RbRoot tree;
    Vma *head;          /* lowest VMA, or NULL */
    Vma *tail;          /* highest VMA, or NULL */
    uint32_t count;
    uint64_t pages;     /* sum of all VMA lengths, in pages (total VM) */
} VmaSet;

/* Empty set. */
void vma_set_init(VmaSet *set);

/* Set up the VMA slab cache (boot, after slab_init). */
void vma_cache_init(void);

/* A zeroed VMA from the slab cache, or NULL. O(1). */
Vma *vma_alloc(void);

/* Return a VMA (already removed from its set) to the cache. */
void vma_free(Vma *vma);

/* First VMA whose end is above addr (it contains addr, or is the next one
 * up), or NULL. O(log n). */
Vma *vma_find(const VmaSet *set, uint64_t addr);

/* The VMA containing addr, or NULL. O(log n). */
Vma *vma_find_containing(const VmaSet *set, uint64_t addr);

/* 1 if no VMA overlaps [start, end). O(log n). */
int vma_range_free(const VmaSet *set, uint64_t start, uint64_t end);

/* Add a VMA; its range must not overlap any VMA in the set. O(log n). */
void vma_insert(VmaSet *set, Vma *vma);

/* Take a VMA out of the set (not freed). O(log n). */
void vma_remove(VmaSet *set, Vma *vma);

/* Move a VMA's bounds without changing its order among the others (the new
 * range must not reach a neighbour). Keeps the page count right. O(1). */
void vma_set_bounds(VmaSet *set, Vma *vma, uint64_t start, uint64_t end);

/* Split vma at addr (page aligned, strictly inside it): vma keeps
 * [start, addr), the returned new VMA holds [addr, end) with the same
 * attributes. Returns NULL (vma unchanged) when out of memory. O(log n). */
Vma *vma_split(VmaSet *set, Vma *vma, uint64_t addr);

/* 1 if b directly follows a and they have the same protection, kind and
 * operation (anonymous only: file VMAs also need contiguous offsets, M1-12). */
int vma_can_merge(const Vma *a, const Vma *b);

/* Merge vma with its neighbours where vma_can_merge allows; the absorbed
 * VMAs are freed. Returns the VMA that now covers vma's range. O(log n). */
Vma *vma_merge_around(VmaSet *set, Vma *vma);

/* Highest address a such that [a, a + len) lies in [low, high), is free and
 * ends at or below `top` (a hint inside [low, high]; pass high for none).
 * Returns 0 when no gap fits. Walks VMAs downward from top: O(log n) to
 * start, then one step per VMA passed (mm.c's hint keeps this short). */
uint64_t vma_find_gap(const VmaSet *set, uint64_t len, uint64_t low, uint64_t high, uint64_t top);

#endif /* ARCHOS_MM_VMA_H */
