#ifndef ARCHOS_ARCH_X86_64_PAGING_H
#define ARCHOS_ARCH_X86_64_PAGING_H

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_PWT        (1ULL << 3)   /* Page-level write-through */
#define PTE_PCD        (1ULL << 4)   /* Page-level cache disable */
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)   /* 2MB page (in PD entry) or 1GB page (in PDPT) */
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NX         (1ULL << 63)  /* No-execute */

/* Mask to extract physical address from PTE (bits 12-51) */
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Entries per table level */
#define PT_ENTRIES     512

/* Index extraction from virtual address */
#define PML4_INDEX(va)  (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va)  (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)    (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)    (((va) >> 12) & 0x1FF)

static inline uint64_t paging_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void paging_write_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static inline void paging_invlpg(uint64_t vaddr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

#endif /* ARCHOS_ARCH_X86_64_PAGING_H */
