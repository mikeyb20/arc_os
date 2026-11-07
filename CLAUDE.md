# arc_os — Project Rules

## Project Description

arc_os is a custom x86_64 operating system built from scratch. Monolithic kernel, POSIX-compatible, Unix-like personality.

**Project goal:** an offline knowledge computer. arc_os installs from a USB key, runs on 4 GB RAM up to a strong desktop, and lets local LLMs (llama.cpp, CPU only) search a large offline library with citations. It works fully without internet.

**The master plan is `docs/plans/knowledge-os.md`. Treat it and the ADRs in `docs/decisions/` as binding.** `overview.md` is history for phases 0–12.

## Non-Negotiables (see ADRs)

- **Syscalls follow the Linux x86_64 ABI** (numbers, semantics, struct layouts). Never invent a syscall number; look it up in the archived `syscall_64.tbl`. Unimplemented → `-ENOSYS`. (ADR-0001)
- System V process start + TLS via FS base; static ELF only. (ADR-0002)
- Kernel code never uses SSE/AVX; user FPU state is saved eagerly with XSAVE. (ADR-0003)
- File I/O and `mmap` share one page cache; per-page metadata comes from the PMM, never kmalloc. (ADR-0004, ADR-0016)
- Drivers register with a bus and plug into block/net/input/fb subsystems; storage is async + DMA. (ADR-0005, ADR-0006)
- Input is Linux `input_event` via `/dev/input`; graphics via `/dev/fb0`; text is UTF-8. (ADR-0007, 0013, 0014)
- Disks: GPT, ESP (FAT32) + system/data (ext2 rev 1). Layout `/system /apps /models /packs /home /var`. (ADR-0008, 0009)
- Programs, models and packs ship as `.arcpkg`; apps reach models only through `arc-llm`. (ADR-0010, 0011, 0012)
- Search and reading must work without the LLM. (ADR-0015)
- Power is first-class: no periodic tick assumptions, no busy-wait idle or endless polling; every driver has suspend/resume hooks. (ADR-0017)
- No scope beyond the plan without Michael's OK. Flag any choice that would force a later rewrite **before** building on it.
- Changing a locked decision = new superseding ADR + Michael's explicit OK + plan update.

## Language & Toolchain

- **Kernel language**: C (C11 standard, C17 where beneficial)
- **Assembly**: NASM syntax for x86_64 assembly stubs (entry points, ISRs, context switch)
- **Build system**: CMake with a freestanding cross-compilation toolchain file
- **Compiler**: `x86_64-elf-gcc` cross-compiler for kernel, system GCC for host-side tests
- **Emulator**: QEMU (`qemu-system-x86_64`)
- **Debugger**: GDB with QEMU remote target (`-s -S` flags)
- **Bootloader**: Limine (modern boot protocol)
- **ISO creation**: xorriso

## Architecture

- **Kernel type**: Monolithic (may evolve to hybrid later)
- **Target**: x86_64 primary, RISC-V as future validation port
- **POSIX compliance**: POSIX-compatible (enables musl, BusyBox, dash ports)
- **License**: MIT

## Directory Structure

```
arc_os/
├── kernel/
│   ├── arch/x86_64/      # Arch-specific (GDT, IDT, PIC, PIT, LAPIC, IOAPIC, SMP, paging, syscall)
│   ├── boot/             # Boot protocol parsing (BootInfo, Limine)
│   ├── mm/               # Memory management (PMM, VMM, kmalloc)
│   ├── proc/             # Process & thread management, scheduler, signals, sync primitives, ELF loader
│   ├── fs/               # VFS layer, ramfs, devfs, procfs, pipe, fat32, path utils, passwd
│   ├── drivers/          # Device drivers (pci, virtio, blkdev, tty, keyboard, fb_console, ansi, vt, acpi)
│   ├── include/          # Freestanding C headers + limine.h
│   ├── net/              # Network stack (ethernet, arp, ipv4, icmp, udp, tcp, socket, loopback)
│   ├── security/         # Hardening (stack canaries, guard pages)
│   └── lib/              # Kernel utility library (mem, string, kprintf)
├── libc/                 # Minimal C library (libarc.a) for user-space programs
│   ├── include/          # POSIX-compatible headers (stdio.h, stdlib.h, unistd.h, etc.)
│   └── src/              # CRT0, string, printf, malloc, syscall wrappers
├── userland/             # User-space programs
│   ├── init/             # Init process (PID 1)
│   ├── login/            # Login authentication
│   ├── shell/            # Interactive shell (27 builtins)
│   ├── hello/            # Hello world
│   ├── echo/             # Echo utility
│   └── coreutils/        # 19 standalone utilities (cat, ls, grep, ps, etc.)
├── tools/                # Build tools, image creation scripts
├── tests/                # Host-side unit tests (47 suites)
├── docs/                 # plans/ (master plan), decisions/ (ADRs), deferred items
└── CMakeLists.txt
```

## Naming Conventions

- **Functions**: `snake_case` — e.g., `pmm_alloc_page()`, `hal_init()`
- **Types/Structs**: `PascalCase` or `snake_case_t` — e.g., `BootInfo`, `thread_t`
- **Constants/Macros**: `UPPER_SNAKE_CASE` — e.g., `PAGE_SIZE`, `GFP_KERNEL`
- **Files**: `snake_case.c` / `snake_case.h`
- **Subsystem prefixes**: Functions are prefixed with their subsystem — `pmm_`, `vmm_`, `hal_`, `vfs_`, `sched_`, `lapic_`, `ioapic_`, `ansi_`, `vt_`

## Abstraction Boundary Rules

- Layers only call downward through defined interfaces, never reach into another layer's internals
- Every subsystem exposes a header with its public API; internals are in separate files
- The HAL isolates all architecture-specific code — everything above the HAL is portable
- The kernel never knows which bootloader was used — it reads a `BootInfo` struct
- User programs link against libc, never invoke syscalls directly

## Code Style

- Braces on same line for functions and control structures (K&R style)
- 4-space indentation, no tabs
- Header guards: `#ifndef ARCHOS_SUBSYSTEM_FILE_H` / `#define ARCHOS_SUBSYSTEM_FILE_H`
- Keep assembly stubs minimal (<100 lines per file); transition to C as fast as possible
- Every public API function gets a brief doc comment in its header

## Build & Run

- `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake && cmake --build build` to build
- `qemu-system-x86_64` with `-serial stdio -s -S` for debug, `-serial stdio` for normal run
- Host-side tests build with system GCC and run natively

## Development Workflow

1. Edit code
2. Build with CMake
3. Boot in QEMU
4. Check serial output (primary debug channel)
5. Debug with GDB if needed (connect to QEMU remote target)

## Milestone Tracking

Progress is tracked as milestones M0–M5 in `docs/plans/knowledge-os.md` (§8), built as **work blocks** listed in `docs/plans/work-blocks.md` (order, size, status). One block = one session = one PR; specs live in `docs/plans/blocks/` (M2–M5 specs are written by each milestone's planning block). Start a block with `/os/implement-block <ID>`, read only what its spec lists, and follow the session rules in work-blocks.md §3. Every milestone ends with an automated QEMU or real-hardware test. `overview.md` phases 0–12 are history.

## Key Docs

- **Master plan:** `docs/plans/knowledge-os.md`
- **Build order and block status:** `docs/plans/work-blocks.md` (`/os/block-status`)
- **Decisions (binding):** `docs/decisions/` (ADR-0001 … ADR-0017)
- Skills for repeated work: `add-syscall`, `driver-bringup`, `port-package` (in `.claude/skills/`)

- Workflow philosophy: `docs/WORKFLOW.md`
- Skill specifications: `docs/SKILL-MAP.md`
- Development plans: `docs/plans/`
- Architecture decisions: `docs/decisions/`
- Session handoff: the Notes section of the block file (`scratchpad*.md` is gitignored and lost when a cloud session ends)
