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
#define VMA_MAYWRITE   (1u << 9)   /* shared file mapping whose file is open for writing: mprotect may add write */

struct Vma;
struct Mm;

/* Operations of a VMA backed by an object (a file, M1-12). Anonymous VMAs
 * have none: mm.c maps zeroed pages. */
typedef struct VmaOps {
    /* Resolve a fault at the page addr (mm lock held for reading; the VMA
     * allows the access). Returns an MM_FAULT_* result (mm.h). */
    int (*fault)(struct Mm *mm, struct Vma *vma, uint64_t addr, uint32_t access);
    /* A new VMA now covers part of the same object (split, fork, mremap):
     * take its references and link it where the object lists its VMAs. */
    void (*open)(struct Vma *vma);
    /* The VMA is being freed (vma_free): undo open. */
    void (*close)(struct Vma *vma);
    /* msync(MS_SYNC) of [start, end) inside a shared VMA: write back and
     * wait. 0 or -errno. NULL: nothing to do. */
    int (*sync)(struct Vma *vma, uint64_t start, uint64_t end);
    /* MADV_WILLNEED of [start, end): read it in now. NULL: nothing to do. */
    void (*willneed)(struct Vma *vma, uint64_t start, uint64_t end);
} VmaOps;

typedef struct Vma {
    RbNode rb;              /* in VmaSet.tree, keyed by start */
    struct Vma *prev;       /* sorted list: lower neighbour, or NULL */
    struct Vma *next;       /* higher neighbour, or NULL */
    uint64_t start;         /* first byte, page aligned */
    uint64_t end;           /* one past the last byte, page aligned */
    uint32_t prot;          /* VMA_READ | VMA_WRITE | VMA_EXEC */
    uint32_t flags;         /* VMA_ANON ... */
    void *file;             /* backing File (a reference; M1-12); NULL when anonymous */
    uint64_t pgoff;         /* file offset of start, in pages (M1-12) */
    const VmaOps *ops;      /* NULL for anonymous memory */
    struct Mm *mm;          /* owning address space (file VMAs: truncate and write-back reach it) */
    struct Vma *as_next;    /* the file's list of VMAs mapping it (mm/filemap.h, under its mapping lock) */
    struct Vma *as_prev;
} Vma;

/* Allocated from its own slab cache: keep it small (ADR-0019). 96 bytes in
 * M1-07; 120 since M1-12 (the owner and the file's VMA-list links, which
 * truncate and write-back walk without any mm lock). */
_Static_assert(sizeof(Vma) <= 120, "struct Vma must stay within 120 bytes");

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

/* Return a VMA (already removed from its set) to the cache; calls its
 * ops->close first. */
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
 * attributes (a file VMA's offset moved on, ops->open called). Returns NULL
 * (vma unchanged) when out of memory. O(log n). */
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
