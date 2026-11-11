# Helpers: one home per job (ADR-0020)

Before writing a helper, find its job here, then `git grep` its name and one distinctive line of its body. Use
the existing helper; if a second copy exists, promote it to the shared header in your PR and update this table.
Kernel and user space are separate worlds: the same function in `kernel/lib` and `libc` is not a duplicate.
`tools/hygiene.py` counts duplicated code and copied `static` helpers; CI fails if either count rises.

## Kernel

| Job | Helper | Header |
|---|---|---|
| Memory copy, fill, compare | `memcpy`, `memset`, `memmove`, `memcmp` | `kernel/lib/mem.h` |
| Strings | `strlen`, `strcmp`, `strncmp`, `strncpy`, `strchr` | `kernel/lib/string.h` |
| Formatted output, log levels, format into a buffer | `kprintf`, `kprintf_level`, `kdebug` (hot paths), `ksnprintf`, `kvsnprintf` | `kernel/lib/kprintf.h` |
| Kernel log ring (`/proc/kmsg` text), console owner | `klog_read`, `klog_tail`, `klog_head`, `klog_console_try_acquire` / `_release`, `klog_set_console_level` | `kernel/lib/klog.h` |
| Kernel command line | `cmdline_get`, `cmdline_raw` | `kernel/lib/cmdline.h` |
| Test mode | `testmode_*` | `kernel/lib/testmode.h` |
| Panic stop with report and screen | `KERNEL_PANIC()` (`kernel/lib/kprintf.h`); exception paths: `panic_begin`, `panic_report`, `hal_panic_capture` | `kernel/lib/panic.h` |
| Kernel address to function name | `ksym_lookup` | `kernel/lib/ksyms.h` |
| Budget metric (CI) | `ARC_BUDGET(name, value)` | `kernel/lib/budget.h` |
| Path clean-up (`.`, `..`, cwd) | `path_normalize` | `kernel/fs/path.h` |
| Physical ↔ virtual (HHDM) | `vmm_get_hhdm_offset()` (+ phys) | `kernel/mm/vmm.h` |
| Page mapping, address spaces | `vmm_map_page[_in]`, `vmm_get_phys[_in]`, `vmm_create_user_pml4`, ... | `kernel/mm/vmm.h` |
| Physical pages (buddy, 4 KiB-4 MiB blocks, `GFP_DMA32`) | `alloc_pages`, `free_pages`; wrappers `pmm_alloc_page`, `pmm_free_page`, `pmm_alloc_contiguous` | `kernel/mm/pmm.h` |
| Page descriptors (`struct page`), PFN / phys / HHDM conversions | `pfn_to_page`, `page_to_pfn`, `page_to_phys`, `page_to_virt`, `virt_to_page`, `pfn_valid` | `kernel/mm/page.h` |
| Allocation flags | `GFP_KERNEL`, `GFP_ZERO`, `GFP_DMA32` | `kernel/mm/gfp.h` |
| Size strings (`4G`, `512M`) | `pmm_parse_size` | `kernel/mm/pmm.h` |
| Kernel heap (16 B-4 MiB, no header) | `kmalloc`, `kfree`, `krealloc` | `kernel/mm/kmalloc.h` |
| Object caches for hot structures | `kmem_cache_create`, `kmem_cache_alloc`, `kmem_cache_free`, `kmem_cache_shrink`; `slab_owner` | `kernel/mm/slab.h` |
| PFN-linked page lists (O(1) push/remove through `lru`) | `page_list_push`, `page_list_remove`, `page_list_first` | `kernel/mm/page.h` |
| User memory | `copy_from_user`, `copy_to_user`, `strncpy_from_user` | `kernel/include/user_access.h` |
| Spinlock (IRQ-safe) | `spinlock_acquire`, `spinlock_release` | `kernel/proc/spinlock.h` |
| Interrupts off/on around a section | `irq_save`, `irq_restore` | `kernel/proc/spinlock.h` |
| Sleeping locks and waits | mutex, semaphore, condvar, `wq_sleep` / `wq_wake` | `kernel/proc/{mutex,semaphore,condvar,waitqueue}.h` |
| Close an fd, copy an fd (pipe/socket refs) | `fd_close`, `fd_file_addref` | `kernel/proc/fd.h` |
| End the current process (exit, fatal signal) | `proc_exit` | `kernel/proc/proc_syscalls.h` |
| Hardware access from portable code | `hal_*`, FPU state, CPUID | `kernel/arch/hal.h` |
| Per-CPU block (GS), running thread, entry stacks | `this_cpu`, `percpu_get`, `percpu_index_of_apic`; portable code: `hal_current_thread`, `hal_set_kernel_stack` | `kernel/arch/x86_64/percpu.h` (via `arch/hal.h`) |
| Local APIC (xAPIC or x2APIC): EOI, IPIs, ID | `lapic_eoi`, `lapic_send_ipi`, `lapic_id`, `lapic_x2apic` | `kernel/arch/x86_64/lapic.h` |
| Start another CPU (bootloader-neutral) | `boot_start_cpu` | `kernel/boot/bootinfo.h` |
| Time-stamp counter (raw cycles) | `cpu_rdtsc`; portable code: `hal_tsc_read` | `kernel/arch/x86_64/cpuid.h`, `kernel/arch/hal.h` |
| TSC frequency and cycles to ns (boot calibration) | `hal_tsc_info`, `hal_tsc_to_ns` (`tsc_info`, `tsc_cycles_to_ns`) | `kernel/arch/hal.h` (`arch/x86_64/tsc.h`) |
| Busy-wait hint in a time-bounded loop | `hal_cpu_relax` | `kernel/arch/hal.h` |
| Run a function on every other CPU (bounded wait) | `hal_run_on_other_cpus` (`ipi_call_others`) | `kernel/arch/hal.h` (`arch/x86_64/ipi.h`) |
| CPU brand, signature, feature names | `hal_cpu_brand`, `hal_cpu_signature`, `hal_cpu_feature_names` (`cpu_brand`, `cpu_feature_names`) | `kernel/arch/hal.h` |
| Machine maker and model (SMBIOS) | `smbios_read` | `kernel/drivers/smbios.h` |
| PCI driver binding, class names | `pci_set_driver`, `pci_class_name` | `kernel/drivers/pci.h` |
| One screen row of text (pagers, banners) | `fb_console_write_row` | `kernel/drivers/fb_console.h` |
| Boot report line, TSC-timed delay (diagnostic mode) | `diag_line`, `diag_delay_ms` | `kernel/diag/diag.h` |
| CPU features, x86-64-v3 floor, core type | `cpu_features`, `cpu_floor_missing`, `cpu_core_type_detect`, `cpu_describe` | `kernel/arch/x86_64/cpu.h` (via `arch/hal.h`) |
| Signal actions and masks (rt_sigaction, fork, execve) | `sig_set_action`, `sig_fork_copy`, `sig_exec_reset` | `kernel/proc/signal.h` |
| Linux ABI values and structs | `__NR_*`, errno, flags, `struct stat`, ... | `kernel/include/uapi/` |

## User space

libarc (`libc/include/`, `libc/src/`) is the only C library for our own programs: `stdio.h`, `stdlib.h`,
`string.h`, `unistd.h`, `fcntl.h`, `sys/*.h`. Programs never invoke syscalls directly. Ported programs use musl.

## Build (CMake)

| Job | Helper | Home |
|---|---|---|
| A libarc program: flags, `libc/user.ld`, copy to the build root | `arc_user_program()` | `userland/CMakeLists.txt` |
| A lane test (traced under strace) | `lane_test()`, `lane_driver_test()` | `stack/CMakeLists.txt` |
| Where the pinned musl tools are | `ARC_MUSL_PREFIX` | `cmake/musl-prefix.cmake` |

## Known duplicates (debt, frozen by the hygiene budgets)

Removed in a `tidy:` commit by the first block that edits the file anyway; that PR lowers the limit.

| Duplicate | Where | Removed by |
|---|---|---|
| `phys_to_virt` (static) and a private `hhdm_offset` | `kernel/mm/vmm.c`, `kernel/drivers/acpi.c` (`pmm.c`'s copy went in M1-02; `mm/page.h` now holds `pmm_hhdm_offset`) | first block that edits `vmm.c` (likely M1-07): one `phys_to_virt` inline in `mm/page.h` on `pmm_hhdm_offset`, and `vmm.c`'s private offset goes |
| `mode_to_rwx` | `userland/coreutils/ls.c`, `userland/shell/shell.c` | next block that edits `ls` or the shell |
| Repeated code regions (12) | `python3 tools/hygiene.py` lists them | the block that rewrites each file |
| `irq_save` vs `hal_save_irq_state` (the HAL one doesn't disable interrupts) | `kernel/proc/spinlock.h`, `kernel/arch/hal.h` | M1-05 (lock rework) |
