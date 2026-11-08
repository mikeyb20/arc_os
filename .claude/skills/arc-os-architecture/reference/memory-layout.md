# Memory Layout Reference

The constants live in the headers; this page only points at them (ADR-0020: docs don't restate code).

| What | Where it is defined |
|---|---|
| Page size, page alignment | `PAGE_SIZE`, `PAGE_ALIGN_*` in `kernel/mm/pmm.h` |
| Physical memory direct map (HHDM) | offset from Limine at boot: `vmm_get_hhdm_offset()` in `kernel/mm/vmm.h` (not a fixed constant) |
| Kernel image | linked high (`kernel/arch/x86_64/linker.ld`), placed by Limine |
| Kernel heap | `HEAP_START`, `HEAP_SIZE_MAX` in `kernel/mm/kmalloc.c` |
| User stack | `USER_STACK_TOP`, `USER_STACK_PAGES` in `kernel/mm/vmm.h` |
| User address limit | `USER_ADDR_LIMIT` in `kernel/include/user_access.h` |
| Static-PIE load address | `ELF_PIE_BASE` in `kernel/proc/elf.h` |
| Kernel thread stacks | `THREAD_STACK_SIZE` in `kernel/proc/thread.h`; double-fault IST stack in `kernel/arch/x86_64/gdt.c` |
| Page-table entry bits | `kernel/arch/x86_64/paging.h` |

x86_64 4-level paging: PML4 → PDPT → PD → PT, 9 index bits each, 4 KiB pages. Per-page metadata and the
memory sizing rules come from ADR-0016; the VMA layout arrives with M1-07.
