# Deferred Items

Items intentionally postponed from their original phase. Each entry notes the original phase and the reason for deferral.

## Phase 1: Booting & Early Kernel

- **1.4 Framebuffer console** — Pixel rendering with bitmap font, cursor, scrolling. Serial output is sufficient for current development; framebuffer adds complexity without unlocking new capabilities yet.
- **1.9 PS/2 keyboard** — ~~Deferred~~ **DONE** — IRQ 1 handler, scancode set 1 → ASCII, shift/ctrl/caps tracking. Integrated with TTY subsystem.
- **1.10 HAL consolidation** — Unified `kernel/arch/hal.h` interface wrapping GDT/IDT/PIC/PIT/paging. Current direct calls work fine; abstraction is premature until a second architecture port is attempted.

## Phase 3: Threading & Scheduling

- ~~**Sleep queues**~~ **DONE** — Wait queues (`wq_sleep`/`wq_wake`/`wq_wake_all`) with condition-variable semantics. Converted busy-wait sites in sys_wait, pipes, and TTY to proper sleep/wake.
- **Mutexes / semaphores / condition variables** — Sleeping locks for longer critical sections. Spinlocks suffice for current single-CPU kernel.
- **Thread-local storage (TLS)** — Per-thread kernel data. Not needed until per-CPU data or complex driver state requires it.
- **Work queues** — Deferred work items processed by worker threads. Needed when interrupt handlers must defer complex processing.

## Phase 4: Drivers

- **4.1 ACPI table parsing** — RSDP/RSDT/MADT parsing. Not needed until APIC (for SMP) or power management is implemented.
- **Memory barrier HAL abstraction** — Move `mfence`/`lfence` wrappers from `virtio.c` into the HAL. Premature until a second driver needs them.
- **VirtIO feature caching** — Cache negotiated feature bits in `VirtioDevice` struct. Not needed until feature-dependent driver logic exists (e.g., VirtIO-net multiqueue).
- **DMA buffer pool** — Pre-allocated DMA buffers for VirtIO. Current per-request PMM alloc/free is correct but expensive under heavy I/O; optimize when VFS drives real multi-sector workloads.

## Phase 5: System Calls & User Space

- ~~**fork/exec/wait syscalls (5.5)**~~ **DONE** — fork with vmm_fork_address_space, exec via ELF from VFS, wait with zombie reaping.
- ~~**User pointer validation (copy_from_user/copy_to_user)**~~ **DONE** — user_ptr_valid checks bounds below USER_ADDR_LIMIT.

## Phase 6: File Systems

- **Dentry cache** — Resolved path component caching for performance. Current flat path resolution is adequate for ramfs.
- ~~**Mount table**~~ **DONE** — Multi-mount VFS with 8 mount point slots. ramfs at /, devfs at /dev, procfs at /proc.
- **FAT32 mounting** — FAT32 driver (fat32.c) exists and passes tests, but not yet mounted in VFS. Deferred until real disk I/O workloads need persistent storage.

## Phase 7: IPC & Shell

- **Signal masking (sigprocmask)** — Block/unblock signals per-process. Not needed until complex signal-handling applications require it.
- **sigaction** — Extended signal handler registration with flags (SA_RESTART, SA_SIGINFO). Current signal() interface suffices for basic use.
- **-EINTR for blocked syscalls** — Return EINTR from blocked read/write/wait when interrupted by a signal. Current signal delivery doesn't interrupt sleeping syscalls.
- **sigaltstack** — Alternate signal stack for handling stack overflow signals. Not needed for current simple signal use cases.
- **Process groups / killpg** — Group processes for job control. Not needed until job control (bg, fg, Ctrl+Z) is implemented.
- **ISR return path signal check** — Check for pending signals when returning from interrupt (not just syscall). Current syscall-return-only delivery is sufficient.
