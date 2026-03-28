# Deferred Items

Items intentionally postponed from their original phase. Each entry notes the original phase and the reason for deferral.

## Phase 1: Booting & Early Kernel

- ~~**1.4 Framebuffer console**~~ **DONE** — fb_console driver with 8x16 VGA bitmap font, cursor tracking, line wrap, scrolling, tab/backspace support. kprintf output hook sends to both serial and framebuffer. Wired into boot sequence.
- **1.9 PS/2 keyboard** — ~~Deferred~~ **DONE** — IRQ 1 handler, scancode set 1 → ASCII, shift/ctrl/caps tracking. Integrated with TTY subsystem.
- ~~**1.10 HAL consolidation**~~ **DONE** — Unified `kernel/arch/hal.h` facade wrapping GDT/IDT/PIC/PIT/paging/IO/serial/MSR with grouped HAL API (CPU control, IRQ management, timer, paging, memory barriers).

## Phase 3: Threading & Scheduling

- ~~**Sleep queues**~~ **DONE** — Wait queues (`wq_sleep`/`wq_wake`/`wq_wake_all`) with condition-variable semantics. Converted busy-wait sites in sys_wait, pipes, and TTY to proper sleep/wake.
- ~~**Mutexes / semaphores / condition variables**~~ **DONE** — Sleeping locks (mutex.c, semaphore.c, condvar.c) built on spinlock + wait queue. Mutex with trylock, counting semaphore with trywait/getvalue, condvar with signal/broadcast.
- **Thread-local storage (TLS)** — Per-thread kernel data. Not needed until per-CPU data or complex driver state requires it.
- **Work queues** — Deferred work items processed by worker threads. Needed when interrupt handlers must defer complex processing.

## Phase 4: Drivers

- ~~**4.1 ACPI table parsing**~~ **DONE** — RSDP v1/v2 validation, RSDT (32-bit) and XSDT (64-bit) parsing, MADT parsing (Local APIC, I/O APIC, Interrupt Source Overrides). Wired into boot sequence.
- **Memory barrier HAL abstraction** — Move `mfence`/`lfence` wrappers from `virtio.c` into the HAL. Premature until a second driver needs them.
- **VirtIO feature caching** — Cache negotiated feature bits in `VirtioDevice` struct. Not needed until feature-dependent driver logic exists (e.g., VirtIO-net multiqueue).
- **DMA buffer pool** — Pre-allocated DMA buffers for VirtIO. Current per-request PMM alloc/free is correct but expensive under heavy I/O; optimize when VFS drives real multi-sector workloads.

## Phase 5: System Calls & User Space

- ~~**fork/exec/wait syscalls (5.5)**~~ **DONE** — fork with vmm_fork_address_space, exec via ELF from VFS, wait with zombie reaping.
- ~~**User pointer validation (copy_from_user/copy_to_user)**~~ **DONE** — user_ptr_valid checks bounds below USER_ADDR_LIMIT.

## Phase 6: File Systems

- **Dentry cache** — Resolved path component caching for performance. Current flat path resolution is adequate for ramfs.
- ~~**Mount table**~~ **DONE** — Multi-mount VFS with 8 mount point slots. ramfs at /, devfs at /dev, procfs at /proc.
- ~~**FAT32 mounting**~~ **DONE** — FAT32 driver wired into VFS via `virtio_blk_setup()` in kmain.c. Mounts at `/disk` when VirtIO-blk device has a FAT32 filesystem.

## Phase 7: IPC & Shell

- ~~**Process groups / killpg**~~ **DONE** — Process groups (pgid field), sig_send_group, setpgid/getpgid/tcsetpgrp syscalls, Ctrl+Z job control.
- **Signal masking (sigprocmask)** — Block/unblock signals per-process. Not needed until complex signal-handling applications require it.
- **sigaction** — Extended signal handler registration with flags (SA_RESTART, SA_SIGINFO). Current signal() interface suffices for basic use.
- **-EINTR for blocked syscalls** — Return EINTR from blocked read/write/wait when interrupted by a signal. Current signal delivery doesn't interrupt sleeping syscalls.
- **sigaltstack** — Alternate signal stack for handling stack overflow signals. Not needed for current simple signal use cases.
- **ISR return path signal check** — Check for pending signals when returning from interrupt (not just syscall). Current syscall-return-only delivery is sufficient.

## Phase 8: Networking

- ~~**Socket API (8.3)**~~ **DONE** — BSD socket interface with 9 syscalls (socket/bind/listen/accept/connect/send/recv/sendto/recvfrom). Sockets as file descriptors, blocking I/O via wait queues, ephemeral port allocation.
- ~~**TCP protocol**~~ **DONE** — Minimal TCP: 3-way handshake (SYN/SYN-ACK/ACK), data transfer with ACK, FIN/RST handling, TCP checksum with pseudo-header. State machine (CLOSED through TIME_WAIT).
- ~~**UDP protocol**~~ **DONE** — Datagram delivery via socket API, per-datagram metadata in rx buffer for recvfrom.
- **DHCP client** — Automatic IP configuration. Currently using static IP (10.0.2.15) with QEMU user-mode networking defaults.
- **DNS resolver** — Hostname resolution. Not needed until user-space programs need name resolution.
- ~~**Loopback interface**~~ **DONE** — lo0 at 127.0.0.1/8, loopback_send bypasses Ethernet and feeds directly to ipv4_rx.

## Phase 9: Security & Permissions

- ~~**/etc/passwd and /etc/group**~~ **DONE** — passwd parser (passwd.c) with colon-delimited format, find-by-name/uid. /etc/passwd created in ramfs at boot with root and user accounts.
- ~~**Login authentication**~~ **DONE** — Login program (userland/login/) reads /etc/passwd, prompts for username/password, sets uid/gid, chdir to home, execs user shell. Init execs login as first program.
- ~~**Setuid/setgid binaries**~~ **DONE** — sys_exec checks S_ISUID/S_ISGID bits on executable, updates euid/egid accordingly.
- ~~**umask per-process**~~ **DONE** — Process.umask field (default 022), inherited on fork, applied in vfs_open (O_CREAT) and vfs_mkdir. SYS_UMASK syscall (34) implemented.
- ~~**Kernel hardening (9.3)**~~ **DONE (partial)** — Stack canaries (`-fstack-protector-strong` + `__stack_chk_fail`), guard page infrastructure (`hardening_add_guard_page`), RDTSC-seeded canary value. Deferred: ASLR, KASLR, full W^X linker script enforcement, syscall sanitization.
