#include "mm/vmm.h"
#include "mm/pmm.h"
#include "arch/x86_64/paging.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* Linker symbols */
extern char _kernel_start[];
extern char _kernel_end[];

static uint64_t kernel_pml4_phys;
static uint64_t hhdm_offset;

/* Convert physical to virtual via HHDM */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

/* Allocate a zeroed page for page table use */
static uint64_t alloc_table_page(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        kprintf("[VMM] FATAL: out of memory for page table\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* Convert portable VMM flags to x86_64 PTE flags */
static uint64_t vmm_flags_to_pte(uint32_t flags) {
    uint64_t pte = PTE_PRESENT;
    if (flags & VMM_FLAG_WRITABLE) pte |= PTE_WRITABLE;
    if (flags & VMM_FLAG_USER)     pte |= PTE_USER;
    if (flags & VMM_FLAG_NOEXEC)   pte |= PTE_NX;
    return pte;
}

/* Ensure a page table entry at table[index] points to a valid next-level table.
 * Returns virtual pointer to the next-level table entries. */
static uint64_t *ensure_table(uint64_t *table, uint64_t index) {
    if (!(table[index] & PTE_PRESENT)) {
        uint64_t new_table = alloc_table_page();
        /* Intermediate tables get Present + Writable + User (most permissive;
         * leaf entries apply the actual restrictions) */
        table[index] = new_table | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    return (uint64_t *)phys_to_virt(table[index] & PTE_ADDR_MASK);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint32_t flags) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    uint64_t *pdpt = ensure_table(pml4, PML4_INDEX(virt));
    uint64_t *pd   = ensure_table(pdpt, PDPT_INDEX(virt));
    uint64_t *pt   = ensure_table(pd,   PD_INDEX(virt));

    pt[PT_INDEX(virt)] = phys | vmm_flags_to_pte(flags);
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) return;

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) return;

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT)) return;

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(virt)] & PTE_ADDR_MASK);
    pt[PT_INDEX(virt)] = 0;

    paging_invlpg(virt);
}

uint64_t vmm_get_phys(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) return 0;

    /* Check for 1GB huge page */
    if (pdpt[PDPT_INDEX(virt)] & PTE_HUGE) {
        return (pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK) + (virt & 0x3FFFFFFF);
    }

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT)) return 0;

    /* Check for 2MB huge page */
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        return (pd[PD_INDEX(virt)] & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
    }

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pt[PT_INDEX(virt)] & PTE_PRESENT)) return 0;

    return (pt[PT_INDEX(virt)] & PTE_ADDR_MASK) + (virt & 0xFFF);
}

uint64_t vmm_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}

uint64_t vmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

/* Map a range using 2MB huge pages where possible, 4K pages otherwise */
static void map_range_2mb(uint64_t virt_start, uint64_t phys_start,
                          uint64_t size, uint32_t flags) {
    uint64_t pte_flags = vmm_flags_to_pte(flags);
    uint64_t offset = 0;

    while (offset < size) {
        uint64_t virt = virt_start + offset;
        uint64_t phys = phys_start + offset;
        uint64_t remaining = size - offset;

        /* Try 2MB page if aligned and enough remaining */
        if ((virt & 0x1FFFFF) == 0 && (phys & 0x1FFFFF) == 0 && remaining >= 0x200000) {
            /* Ensure PML4 and PDPT entries exist */
            uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
            if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) {
                uint64_t new_table = alloc_table_page();
                pml4[PML4_INDEX(virt)] = new_table | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            }
            uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
            if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) {
                uint64_t new_table = alloc_table_page();
                pdpt[PDPT_INDEX(virt)] = new_table | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            }
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);

            /* Set as 2MB huge page */
            pd[PD_INDEX(virt)] = phys | pte_flags | PTE_HUGE;
            offset += 0x200000;
        } else {
            /* Fall back to 4K page */
            vmm_map_page(virt, phys, flags);
            offset += PAGE_SIZE;
        }
    }
}

void vmm_init(const BootInfo *info) {
    hhdm_offset = info->hhdm_offset;

    /* Allocate a fresh PML4 */
    kernel_pml4_phys = alloc_table_page();
    kprintf("[VMM] New PML4 at phys 0x%lx\n", kernel_pml4_phys);

    /* 1. Map the HHDM: identity-map all physical memory at hhdm_offset.
     *    We need to cover all physical memory that might be referenced. */
    uint64_t highest_phys = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        uint64_t top = info->memory_map[i].base + info->memory_map[i].length;
        if (top > highest_phys) highest_phys = top;
    }
    /* Round up to 2MB boundary for clean huge page mapping */
    highest_phys = (highest_phys + 0x1FFFFF) & ~0x1FFFFFULL;

    kprintf("[VMM] Mapping HHDM: 0x%lx -> phys 0x0 (%lu MB)\n",
            hhdm_offset, highest_phys / (1024 * 1024));
    map_range_2mb(hhdm_offset, 0, highest_phys, VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);

    /* 2. Map the kernel image at its virtual address.
     *    Kernel runs at KERNEL_VMA (0xFFFFFFFF80000000), loaded at kernel_phys_base. */
    uint64_t kernel_virt = (uint64_t)_kernel_start;
    uint64_t kernel_phys = info->kernel_phys_base;
    uint64_t kernel_size = (uint64_t)_kernel_end - (uint64_t)_kernel_start;
    /* Round up to page boundary */
    kernel_size = (kernel_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    kprintf("[VMM] Mapping kernel: 0x%lx -> phys 0x%lx (%lu KB)\n",
            kernel_virt, kernel_phys, kernel_size / 1024);
    /* Map kernel 4K at a time (it's small, and text/rodata/data have different perms
     * ideally, but for now we just map all writable) */
    for (uint64_t off = 0; off < kernel_size; off += PAGE_SIZE) {
        vmm_map_page(kernel_virt + off, kernel_phys + off, VMM_FLAG_WRITABLE);
    }

    /* 3. Map framebuffer if present */
    if (info->fb_present) {
        uint64_t fb_virt = (uint64_t)info->framebuffer.address;
        /* The framebuffer virtual address from Limine is in the HHDM, which we
         * already mapped. Find its physical address from the memory map. */
        for (uint64_t i = 0; i < info->memory_map_count; i++) {
            if (info->memory_map[i].type == MEMMAP_FRAMEBUFFER) {
                kprintf("[VMM] Framebuffer at phys 0x%lx (covered by HHDM)\n",
                        info->memory_map[i].base);
                break;
            }
        }
        (void)fb_virt;
    }

    /* Switch to our page tables */
    kprintf("[VMM] Switching CR3...\n");
    paging_write_cr3(kernel_pml4_phys);

    kprintf("[VMM] Page tables active. Kernel running on own page tables.\n");
}
