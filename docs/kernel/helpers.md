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
| Formatted output | `kprintf` | `kernel/lib/kprintf.h` |
| Kernel command line | `cmdline_get`, `cmdline_raw` | `kernel/lib/cmdline.h` |
| Test mode, panic stop | `testmode_*`, `kernel_panic_halt` | `kernel/lib/testmode.h` |
| Budget metric (CI) | `ARC_BUDGET(name, value)` | `kernel/lib/budget.h` |
| Path clean-up (`.`, `..`, cwd) | `path_normalize` | `kernel/fs/path.h` |
| Physical ↔ virtual (HHDM) | `vmm_get_hhdm_offset()` (+ phys) | `kernel/mm/vmm.h` |
| Page mapping, address spaces | `vmm_map_page[_in]`, `vmm_get_phys[_in]`, `vmm_create_user_pml4`, ... | `kernel/mm/vmm.h` |
| Physical pages | `pmm_alloc_page`, `pmm_free_page`, `pmm_alloc_contiguous` | `kernel/mm/pmm.h` |
| Kernel heap | `kmalloc`, `kfree`, `krealloc` (slab from M1-02b) | `kernel/mm/kmalloc.h` |
| User memory | `copy_from_user`, `copy_to_user`, `strncpy_from_user` | `kernel/include/user_access.h` |
| Spinlock (IRQ-safe) | `spinlock_acquire`, `spinlock_release` | `kernel/proc/spinlock.h` |
| Interrupts off/on around a section | `irq_save`, `irq_restore` | `kernel/proc/spinlock.h` |
| Sleeping locks and waits | mutex, semaphore, condvar, `wq_sleep` / `wq_wake` | `kernel/proc/{mutex,semaphore,condvar,waitqueue}.h` |
| Hardware access from portable code | `hal_*`, FPU state, CPUID | `kernel/arch/hal.h` |
| Linux ABI values and structs | `__NR_*`, errno, flags, `struct stat`, ... | `kernel/include/uapi/` |

## User space

libarc (`libc/include/`, `libc/src/`) is the only C library for our own programs: `stdio.h`, `stdlib.h`,
`string.h`, `unistd.h`, `fcntl.h`, `sys/*.h`. Programs never invoke syscalls directly. Ported programs use musl.

## Known duplicates (debt, frozen by the hygiene budgets)

Removed in a `tidy:` commit by the first block that edits the file anyway; that PR lowers the limit.

| Duplicate | Where | Removed by |
|---|---|---|
| `phys_to_virt` (static) and a private `hhdm_offset` | `kernel/mm/vmm.c`, `kernel/drivers/acpi.c`, `kernel/mm/pmm.c` | first block that edits `pmm.c` or `vmm.c` (likely M1-02): one inline in `vmm.h` |
| `mode_to_rwx` | `userland/coreutils/ls.c`, `userland/shell/shell.c` | next block that edits `ls` or the shell |
| Repeated code regions (12) | `python3 tools/hygiene.py` lists them | the block that rewrites each file |
| `irq_save` vs `hal_save_irq_state` (the HAL one doesn't disable interrupts) | `kernel/proc/spinlock.h`, `kernel/arch/hal.h` | M1-05 (lock rework) |
