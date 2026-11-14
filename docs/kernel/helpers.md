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
| Kernel log ring (`/proc/kmsg` text), console owner | `klog_read`, `klog_tail`, `klog_head`, `klog_for_each_line` (whole lines, markers stripped, by level), `klog_console_try_acquire` / `_release`, `klog_console_acquire` (bounded wait, for screen drawing outside the log), `klog_set_console_level` | `kernel/lib/klog.h` |
| Build commit (12-digit hash, `-dirty`, or `unknown`) | `arc_build_commit` | `kernel/lib/build_commit.h` |
| Kernel command line | `cmdline_get`, `cmdline_raw` | `kernel/lib/cmdline.h` |
| CRC-32 (IEEE 802.3, zlib-compatible chaining) | `crc32` | `kernel/lib/crc32.h` |
| Test mode | `testmode_*` | `kernel/lib/testmode.h` |
| Panic stop with report and screen | `KERNEL_PANIC()` (`kernel/lib/kprintf.h`); exception paths: `panic_begin`, `panic_report`, `hal_panic_capture` | `kernel/lib/panic.h` |
| Kernel address to function name | `ksym_lookup` | `kernel/lib/ksyms.h` |
| Budget metric (CI) | `ARC_BUDGET(name, value)` | `kernel/lib/budget.h` |
| Physical ↔ virtual (HHDM) | `vmm_get_hhdm_offset()` (+ phys) | `kernel/mm/vmm.h` |
| Page mapping, address spaces | `vmm_map_page[_in]`, `vmm_get_phys[_in]`, `vmm_create_user_pml4`, ... | `kernel/mm/vmm.h` |
| Physical pages (buddy, 4 KiB-4 MiB blocks, `GFP_DMA32`) | `alloc_pages`, `free_pages`; wrappers `pmm_alloc_page`, `pmm_free_page`, `pmm_alloc_contiguous` | `kernel/mm/pmm.h` |
| Buddy order for a byte size | `page_order_for` | `kernel/mm/page.h` |
| Page descriptors (`struct page`), PFN / phys / HHDM conversions | `pfn_to_page`, `page_to_pfn`, `page_to_phys`, `page_to_virt`, `virt_to_page`, `pfn_valid` | `kernel/mm/page.h` |
| Allocation flags | `GFP_KERNEL`, `GFP_ZERO`, `GFP_DMA32` | `kernel/mm/gfp.h` |
| Size strings (`4G`, `512M`) | `pmm_parse_size` | `kernel/mm/pmm.h` |
| Kernel heap (16 B-4 MiB, no header) | `kmalloc`, `kfree`, `krealloc` | `kernel/mm/kmalloc.h` |
| Object caches for hot structures | `kmem_cache_create`, `kmem_cache_alloc`, `kmem_cache_free`, `kmem_cache_shrink`; `slab_owner` | `kernel/mm/slab.h` |
| PFN-linked page lists (O(1) push/remove through `lru`) | `page_list_push`, `page_list_remove`, `page_list_first` | `kernel/mm/page.h` |
| User memory (every user pointer; `-EFAULT`, never a raw dereference) | `copy_from_user`, `copy_to_user`, `strncpy_from_user`, `get_user`, `put_user`; range check only: `user_ptr_valid`; under a page lock (no fault, short copy instead): `copy_from_user_nofault` | `kernel/include/user_access.h` |
| Intrusive red-black tree (caller compares keys) | `rb_link_node`, `rb_insert_color`, `rb_erase`, `rb_first`, `rb_next`, `rb_entry` | `kernel/lib/rbtree.h` |
| Sleeping read-write lock | `rwsem_init`, `rwsem_down_read` / `_up_read`, `rwsem_down_write` / `_up_write`, `rwsem_try_read`, `rwsem_write_held` | `kernel/proc/rwsem.h` |
| VMA sets: lookup, split, merge, gap search | `vma_find`, `vma_find_containing`, `vma_range_free`, `vma_insert`, `vma_remove`, `vma_split`, `vma_merge_around`, `vma_find_gap` | `kernel/mm/vma.h` |
| User address spaces: create, fork, load, map, fault | `mm_create`, `mm_get` / `mm_put`, `mm_fork`, `mm_switch`, `mm_map_anon`, `mm_unmap`, `mm_protect`, `mm_fault`, `mm_setup_vma` / `_page` / `_stack` (exec) | `kernel/mm/mm.h` |
| Flush a user range's TLB entries on every CPU that has the mm loaded | `mm_flush_tlb_range`; this CPU, one address (after a fault): `mm_flush_tlb_local`; portable code below mm: `hal_tlb_flush`, kernel ranges `hal_tlb_flush_kernel` | `kernel/mm/mm.h`, `kernel/arch/hal.h` (`arch/x86_64/tlb.h`) |
| Radix tree keyed by a 64-bit index (6 bits per level: 4 levels below 2^24, a 64 GiB file; slab nodes) | `radix_init`, `radix_lookup`, `radix_insert`, `radix_delete`, `radix_next` | `kernel/mm/radix.h` |
| Page cache: a file's pages, read/write through them, page lock, readahead, write-back, truncate, drop clean pages, counters | `filemap_inode_init` / `_evict`, `filemap_of`, `filemap_size`, `filemap_end_index`, `filemap_get_page`, `filemap_find`, `page_put`, `page_lock` / `page_unlock`, `filemap_end_read`, `filemap_read` / `filemap_write` (a filesystem's fops), `filemap_set_dirty`, `filemap_write_range`, `filemap_fsync`, `filemap_truncate`, `filemap_readahead`, `filemap_drop_clean`, `filemap_stats` | `kernel/mm/filemap.h` |
| Block-device page cache (filesystem metadata); page-cache disk I/O: a page read in pieces, synchronous writes | `filemap_bdev_init` / `_release`; `filemap_io_begin`, `filemap_bdev_read`, `filemap_bdev_read_done` (completion), `filemap_io_end`; `filemap_bdev_write` | `kernel/mm/filemap.h` |
| mmap of a file (VMA ops for `FileOps.mmap_ops`) and object VMAs | `filemap_vma_ops`; `mm_map_object`, `mm_setup_object_vma` (exec), `mm_msync`, `mm_advise_access`, `mm_willneed` | `kernel/mm/filemap.h`, `kernel/mm/mm.h` |
| Load an ELF from memory or from any source (a file: through the page cache) | `elf_load`, `elf_load_from` + `ElfSource`, `elf_source_mem`; from a path: `exec_load` | `kernel/proc/elf.h`, `kernel/proc/proc_syscalls.h` |
| Spinlock (caller keeps the interrupt flags; every lock has a level) | `spin_lock_irqsave` / `spin_unlock_irqrestore`, `spin_lock` / `spin_unlock`, `spin_trylock`, `SPINLOCK_INIT(level, name)`, `spin_lock_init` | `kernel/proc/spinlock.h` |
| Lock levels and the debug lock-order checker | `LOCK_LEVEL_*`, `lockdep_check_order` | `kernel/proc/lockdep.h` (`docs/kernel/locking.md`) |
| Big kernel lock (interim, until subsystems have their own locks) | `kernel_lock`, `kernel_lock_irq`, `kernel_unlock`, `kernel_lock_held` | `kernel/proc/kernel_lock.h` |
| Interrupts off/on around a section; this CPU's spinlock depth | `irq_save`, `irq_restore`, `hal_lock_depth` | `kernel/arch/hal_lock.h` (via `proc/spinlock.h` or `arch/hal.h`) |
| CPU sets (affinity) | `CpuMask`, `cpumask_set`, `cpumask_test`, `cpumask_fill`, ... | `kernel/proc/cpumask.h` |
| Run, wake, pin threads; per-CPU scheduler statistics | `sched_add_thread`, `sched_wake`, `sched_continue`, `sched_set_affinity`, `sched_core_mask`, `sched_cpu_stats`, `sched_wait_off_cpu` | `kernel/proc/sched.h` |
| Sleeping locks and waits | mutex, semaphore, condvar, `wq_sleep` / `wq_wake` | `kernel/proc/{mutex,semaphore,condvar,waitqueue}.h` |
| fd tables: install, look up (with a File reference), close, close-on-exec, fork copy | `fd_table_new` / `_dup` / `_get` / `_put`, `fd_install`, `fd_install_at`, `fd_get`, `fd_close`, `fd_get_cloexec` / `fd_set_cloexec`, `fd_close_on_exec` | `kernel/proc/fd.h` |
| Open files: open by path (`vfs_open_at` from a start directory), new File on an inode (pipes, sockets), references, I/O at the File's offset or at a given one, seek, directory listing | `vfs_open`, `file_new`, `fget_ref` / `fput`, `vfs_read` / `vfs_write`, `vfs_pread` / `vfs_pwrite`, `vfs_llseek`, `vfs_generic_llseek`, `vfs_iterate`, `vfs_ftruncate`, `vfs_fsync`; kernel whole-file write `vfs_write_file` | `kernel/fs/vfs.h` |
| Kernel or user buffers for file I/O (one read/write path for both) | `IoBuf`, `iob_kernel`, `iob_user`, `iob_copy_in` / `_out`, `iob_zero` | `kernel/fs/vfs.h` |
| Inodes: allocate, static, references, inode cache, times, permission, stat | `inode_new`, `inode_init_static`, `igrab` / `iput`, `icache_get` / `icache_add`, `vfs_now`, `vfs_check_perm`, `inode_permission`, `vfs_fill_stat` | `kernel/fs/vfs.h` |
| Directory listing from a filesystem (getdents64 entries, "." and "..") | `dir_emit`, `dir_emit_dots`, `vfs_dtype` | `kernel/fs/vfs.h` |
| Paths and names (walked from "/" or a start `PathLoc`; `.`, `..` and symlinks resolved on the tree): resolve, parent + last name, namespace changes, mounts, a directory's path | `vfs_resolve` / `vfs_resolve_at` (`WALK_FOLLOW`), `loc_get` / `loc_put`, `vfs_resolve_parent_at`, `vfs_mkdir[_at]`, `vfs_unlink[_at]`, `vfs_rmdir[_at]`, `vfs_rename[_at]`, `vfs_stat[_at]`, `vfs_readlink_at`, `vfs_setattr`, `vfs_path_of`, `vfs_mount_root`, `vfs_mount`, `vfs_bind_mount`, `vfs_mount_subdir`, `vfs_format_mounts`, `vfs_drop_caches` | `kernel/fs/vfs.h` |
| A process's working directory (by inode) | `vfs_cwd_get`, `vfs_cwd_set`, `vfs_cwd_copy` (fork), `vfs_cwd_release` (exit) | `kernel/fs/vfs.h` |
| Superblocks: set up, anonymous (pipes, sockets) | `vfs_sb_init`, `vfs_anon_sb` | `kernel/fs/vfs.h` |
| End the current process (exit, fatal signal) | `proc_exit` | `kernel/proc/proc_syscalls.h` |
| Hardware access from portable code | `hal_*`, FPU state, CPUID | `kernel/arch/hal.h` |
| Per-CPU block (GS), running thread, entry stacks | `this_cpu`, `percpu_get`, `percpu_index_of_apic`; portable code: `hal_current_thread`, `hal_set_kernel_stack` | `kernel/arch/x86_64/percpu.h` (via `arch/hal.h`) |
| Local APIC (xAPIC or x2APIC): EOI, IPIs, ID | `lapic_eoi`, `lapic_send_ipi`, `lapic_id`, `lapic_x2apic` | `kernel/arch/x86_64/lapic.h` |
| Start another CPU (bootloader-neutral) | `boot_start_cpu` | `kernel/boot/bootinfo.h` |
| Time-stamp counter (raw cycles) | `cpu_rdtsc`; portable code: `hal_tsc_read` | `kernel/arch/x86_64/cpuid.h`, `kernel/arch/hal.h` |
| TSC frequency and cycles to ns (boot calibration) | `hal_tsc_info`, `hal_tsc_to_ns` (`tsc_info`, `tsc_cycles_to_ns`) | `kernel/arch/hal.h` (`arch/x86_64/tsc.h`) |
| Busy-wait hint in a time-bounded loop | `hal_cpu_relax` | `kernel/arch/hal_lock.h` (via `arch/hal.h`) |
| Run a function on every other CPU (bounded wait) | `hal_run_on_other_cpus` (`ipi_call_others`) | `kernel/arch/hal.h` (`arch/x86_64/ipi.h`) |
| CPU brand, signature, feature names | `hal_cpu_brand`, `hal_cpu_signature`, `hal_cpu_feature_names` (`cpu_brand`, `cpu_feature_names`) | `kernel/arch/hal.h` |
| Machine maker and model (SMBIOS) | `smbios_read` | `kernel/drivers/smbios.h` |
| Driver model: buses, devices, drivers, binding, system/runtime suspend and resume; struct from a member | `bus_register`, `bus_add_device`, `driver_register`, `device_unbind`, `bus_suspend_all`, `bus_resume_all`, `device_driver_name`, `container_of` | `kernel/drivers/bus.h` |
| PCI drivers: ID tables, config access, capabilities, BARs | `pci_register_driver`, `PCI_DEVICE`, `PCI_DEVICE_CLASS`, `pci_match_id`, `pci_read16`/`32`, `pci_write16`/`32`, `pci_find_capability`, `pci_bar_phys`, `pci_bar_map`, `pci_enable_bus_master`, `pci_enable_decode`, `pci_driver_name`, `pci_class_name` | `kernel/drivers/pci.h` |
| PCI interrupts: MSI-X, else MSI, else INTx | `pci_alloc_irq_vectors`, `pci_free_irq_vectors`, `pci_msix_info` (read only, boot report) | `kernel/drivers/pci.h` |
| Device interrupt handlers, affinity, counts | `irq_request`, `irq_free`, `irq_set_affinity`, `irq_get_cpu`, `irq_desc_total`; bus layers: `irq_desc_create`, `irq_desc_destroy` | `kernel/drivers/irq.h` |
| Device vectors and MSI messages (HAL) | `hal_irq_vector_alloc`, `hal_irq_vector_free`, `hal_irq_msi_message`, `hal_irq_msi_target_ok`, `hal_irq_line_routing_known` (`irq_vector_*`, `irq_msi_compose`) | `kernel/arch/hal.h` (`arch/x86_64/irq_vector.h`) |
| Map device registers (uncached), read and write them | `mmio_map`, `mmio_read8`..`64`, `mmio_write8`..`64` | `kernel/drivers/mmio.h` |
| DMA: coherent buffers, streaming maps, device masks | `dma_alloc_coherent`, `dma_free_coherent`, `dma_map_single`, `dma_map_page`, `dma_unmap`, `dma_set_mask`, `DMA_BIT_MASK`, `dma_fits` | `kernel/drivers/dma.h` |
| Legacy virtio-PCI transport: vrings, features, MSI-X vectors | `virtio_init_device`, `virtio_setup_irqs`, `virtio_set_config_vector`, `virtio_set_queue_vector`, `virtio_isr_status`, `virtio_init_queue`, `virtq_*` | `kernel/drivers/virtio.h` |
| One screen row of text (pagers, banners) | `fb_console_write_row`; pixels only, while paused: `fb_console_paint_row` | `kernel/drivers/fb_console.h` |
| Own the screen, draw a 1-bit bitmap scaled (QR codes) | `fb_console_pause` / `_resume`, `fb_console_fill`, `fb_console_draw_bitmap`, `fb_console_width` / `_height` / `_row_height` | `kernel/drivers/fb_console.h` |
| Boot report line, TSC-timed delay or wait bound, key wait, command-line flag (diagnostic and test mode) | `diag_line`, `diag_delay_ms`, `diag_ms_to_cycles`, `diag_wait_key`, `diag_flag_on` | `kernel/diag/diag.h` |
| QR code from bytes (byte mode, level M, versions 1-30; no allocation) | `qr_encode`, `qr_capacity`, `qr_module` | `kernel/diag/qr.h` |
| Text as numbered `ARC1` QR pages: sanitize (no links), split, header and CRCs | `qr_text_sanitize`, `qr_text_paginate`, `qr_text_page` | `kernel/diag/qr_text.h` |
| QR page or panic code on screen; panic and log-tail texts | `qr_screen_show_page`, `qr_screen_draw_panic`, `qr_screen_scale`, `qr_screen_page_height`, `qr_panic_encode`, `qr_panic_text`, `qr_log_text` | `kernel/diag/qr_screen.h` |
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
