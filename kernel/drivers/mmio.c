/* arc_os — MMIO mappings (M1-09)
 *
 * Owns: the kernel's MMIO window (HAL_MMIO_BASE, HAL_MMIO_SIZE): a bump
 * allocator of virtual pages mapped uncached onto device memory.
 * Invariants: window space is handed out once and never returned; every
 * handed-out page is mapped before mmio_map returns it. The window's page
 * tables hang under the kernel's top-level entry, which every address space
 * shares, so no CPU needs a TLB flush (the pages were never mapped before).
 * Locks: none of its own (one atomic add per mapping); vmm_map_page takes the
 * kernel page-table lock.
 * Cost: O(pages) per mapping, at probe time only. */

#include "drivers/mmio.h"
#include "arch/hal.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/kprintf.h"

static uint64_t window_used;

volatile void *mmio_map(uint64_t phys, uint64_t size) {
    if (size == 0) return NULL;
    uint64_t first = PAGE_ALIGN_DOWN(phys);
    uint64_t span = PAGE_ALIGN_UP(phys + size) - first;
    uint64_t off = __atomic_fetch_add(&window_used, span, __ATOMIC_RELAXED);
    if (off + span > HAL_MMIO_SIZE || off + span < off) {
        kprintf("[MMIO] window full: cannot map 0x%lx (+%lu bytes)\n", phys, size);
        return NULL;
    }
    uint64_t virt = HAL_MMIO_BASE + off;
    for (uint64_t p = 0; p < span; p += PAGE_SIZE) {
        vmm_map_page(virt + p, first + p, VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC | VMM_FLAG_UNCACHED);
        if (vmm_get_phys(virt + p) != first + p) {   /* vmm_map_page can't report a failed table allocation */
            kprintf("[MMIO] out of memory mapping 0x%lx\n", phys);
            return NULL;
        }
    }
    return (volatile void *)(virt + (phys - first));
}

uint64_t mmio_window_used(void) {
    uint64_t used = __atomic_load_n(&window_used, __ATOMIC_RELAXED);
    return used < HAL_MMIO_SIZE ? used : HAL_MMIO_SIZE;
}
