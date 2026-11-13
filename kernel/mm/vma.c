/* arc_os — VMA sets: tree + sorted list of virtual memory areas (vma.h, M1-07)
 *
 * Owns: the VMA slab cache and the tree/list bookkeeping of every VmaSet.
 * Invariants: in a set, VMAs are non-overlapping and page aligned with
 * start < end; the list is sorted by start and holds exactly the tree's
 * nodes; head/tail are the lowest/highest; count and pages match the
 * contents (tests/test_vma.c checks all of these after every operation).
 * Locks: none here. The owning mm's lock (LOCK_LEVEL_MM) is held for writing
 * around changes and at least for reading around lookups (mm.c).
 * Cost: lookup, insert, remove, split O(log n); merge O(log n) per neighbour;
 * gap search O(log n) plus one step per VMA it passes. */

#include "mm/vma.h"
#include "mm/slab.h"
#include "mm/gfp.h"

#define VMA_PAGE_SHIFT 12

static struct kmem_cache *vma_cache;

void vma_cache_init(void) {
    vma_cache = kmem_cache_create("vma", sizeof(Vma), 8);
}

Vma *vma_alloc(void) {
    return vma_cache != NULL ? kmem_cache_alloc(vma_cache, GFP_ZERO) : NULL;
}

void vma_free(Vma *vma) {
    kmem_cache_free(vma_cache, vma);
}

void vma_set_init(VmaSet *set) {
    set->tree.node = NULL;
    set->head = set->tail = NULL;
    set->count = 0;
    set->pages = 0;
}

static uint64_t vma_pages(uint64_t start, uint64_t end) {
    return (end - start) >> VMA_PAGE_SHIFT;
}

Vma *vma_find(const VmaSet *set, uint64_t addr) {
    /* Non-overlapping ranges sort the same by start and by end */
    RbNode *n = set->tree.node;
    Vma *best = NULL;
    while (n != NULL) {
        Vma *v = rb_entry(n, Vma, rb);
        if (v->end > addr) {
            best = v;
            if (v->start <= addr) break;   /* contains it */
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return best;
}

Vma *vma_find_containing(const VmaSet *set, uint64_t addr) {
    Vma *v = vma_find(set, addr);
    return (v != NULL && v->start <= addr) ? v : NULL;
}

int vma_range_free(const VmaSet *set, uint64_t start, uint64_t end) {
    Vma *v = vma_find(set, start);
    return v == NULL || v->start >= end;
}

void vma_insert(VmaSet *set, Vma *vma) {
    RbNode **link = &set->tree.node;
    RbNode *parent = NULL;
    Vma *prev = NULL;   /* the highest VMA below vma, found on the way down */
    while (*link != NULL) {
        parent = *link;
        Vma *v = rb_entry(parent, Vma, rb);
        if (vma->start < v->start) {
            link = &parent->left;
        } else {
            prev = v;
            link = &parent->right;
        }
    }
    rb_link_node(&vma->rb, parent, link);
    rb_insert_color(&vma->rb, &set->tree);

    vma->prev = prev;
    vma->next = prev != NULL ? prev->next : set->head;
    if (vma->next != NULL)
        vma->next->prev = vma;
    else
        set->tail = vma;
    if (prev != NULL)
        prev->next = vma;
    else
        set->head = vma;
    set->count++;
    set->pages += vma_pages(vma->start, vma->end);
}

void vma_remove(VmaSet *set, Vma *vma) {
    rb_erase(&vma->rb, &set->tree);
    if (vma->prev != NULL)
        vma->prev->next = vma->next;
    else
        set->head = vma->next;
    if (vma->next != NULL)
        vma->next->prev = vma->prev;
    else
        set->tail = vma->prev;
    vma->prev = vma->next = NULL;
    set->count--;
    set->pages -= vma_pages(vma->start, vma->end);
}

void vma_set_bounds(VmaSet *set, Vma *vma, uint64_t start, uint64_t end) {
    set->pages -= vma_pages(vma->start, vma->end);
    vma->start = start;
    vma->end = end;
    set->pages += vma_pages(start, end);
}

Vma *vma_split(VmaSet *set, Vma *vma, uint64_t addr) {
    Vma *upper = vma_alloc();
    if (upper == NULL) return NULL;
    *upper = *vma;   /* attributes; links are set by vma_insert */
    upper->start = addr;
    if (upper->file != NULL) upper->pgoff += vma_pages(vma->start, addr);
    vma_set_bounds(set, vma, vma->start, addr);
    vma_insert(set, upper);
    return upper;
}

int vma_can_merge(const Vma *a, const Vma *b) {
    return a->end == b->start && a->prot == b->prot && a->flags == b->flags && a->ops == b->ops && a->file == NULL &&
           b->file == NULL;
}

Vma *vma_merge_around(VmaSet *set, Vma *vma) {
    Vma *prev = vma->prev;
    if (prev != NULL && vma_can_merge(prev, vma)) {
        uint64_t end = vma->end;
        vma_remove(set, vma);
        vma_free(vma);
        vma_set_bounds(set, prev, prev->start, end);
        vma = prev;
    }
    Vma *next = vma->next;
    if (next != NULL && vma_can_merge(vma, next)) {
        uint64_t end = next->end;
        vma_remove(set, next);
        vma_free(next);
        vma_set_bounds(set, vma, vma->start, end);
    }
    return vma;
}

uint64_t vma_find_gap(const VmaSet *set, uint64_t len, uint64_t low, uint64_t high, uint64_t top) {
    if (len == 0 || high < low || high - low < len) return 0;
    if (top > high || top < low) top = high;

    /* The highest VMA starting below top */
    Vma *v = vma_find(set, top);
    if (v == NULL)
        v = set->tail;
    else if (v->start >= top)
        v = v->prev;

    while (top - low >= len) {
        uint64_t gap_low = (v != NULL && v->end > low) ? v->end : low;
        if (gap_low <= top && top - gap_low >= len) return top - len;
        if (v == NULL || v->start <= low) break;
        if (v->start < top) top = v->start;
        v = v->prev;
    }
    return 0;
}
