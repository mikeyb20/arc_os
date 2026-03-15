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
- **Mount table** — Maps mount points to filesystem instances. Needed when multiple filesystems coexist.
