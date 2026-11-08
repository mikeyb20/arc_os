/* arc_os — memory syscalls (Linux x86_64 numbers, ADR-0001).
 *
 * Moved out of arch/x86_64/syscall.c by M0-05. */

#include "mm/mm_syscalls.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/process.h"
#include "lib/mem.h"
#include "arch/x86_64/syscall.h"
#include "uapi/unistd.h"

/* Highest break: 1 MiB below the user stack, so the heap can never map over
 * the stack or reach kernel addresses. M1-07 replaces this with VMA checks. */
#define BRK_LIMIT (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE) - 0x100000ULL)

/* Unmap and free the heap pages in [from, to) (page-aligned). */
static void brk_unmap(Process *p, uint64_t from, uint64_t to) {
    for (uint64_t vaddr = from; vaddr < to; vaddr += PAGE_SIZE) {
        uint64_t phys = vmm_get_phys_in(p->page_table, vaddr);
        if (phys == 0) continue;
        vmm_unmap_page_in(p->page_table, vaddr);
        pmm_free_page(phys & ~(PAGE_SIZE - 1ULL));
    }
}

/* brk(addr) — 12. Linux semantics: returns the new break on success and the
 * unchanged break on any failure (never -errno); brk(0) queries it. Growing maps
 * zeroed pages; shrinking unmaps and frees whole pages above the new break.
 * Cost: one page allocation or free per page crossed. */
static int64_t sys_brk(uint64_t new_brk, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return 0;

    uint64_t cur = p->brk_current;
    if (new_brk < p->brk_start || new_brk > BRK_LIMIT) return (int64_t)cur;

    uint64_t old_top = PAGE_ALIGN_UP(cur);
    uint64_t new_top = PAGE_ALIGN_UP(new_brk);
    uint64_t hhdm = vmm_get_hhdm_offset();

    if (new_top > old_top) {
        for (uint64_t vaddr = old_top; vaddr < new_top; vaddr += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) {
                brk_unmap(p, old_top, vaddr);   /* undo this call's pages */
                return (int64_t)cur;
            }
            memset((void *)(phys + hhdm), 0, PAGE_SIZE);
            vmm_map_page_in(p->page_table, vaddr, phys,
                            VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
        }
    } else if (new_top < old_top) {
        brk_unmap(p, new_top, old_top);
    }

    p->brk_current = new_brk;
    return (int64_t)new_brk;
}

void mm_syscalls_init(void) {
    syscall_register(__NR_brk, sys_brk);
}
