# arc_os — Project Rules

## Project Description

arc_os is a custom x86_64 operating system built from scratch. Monolithic kernel, POSIX-compatible, Unix-like personality. The goal is a fully functional OS that can boot on real hardware, run a shell, and execute standard Unix utilities.

See `overview.md` for the full 13-phase development roadmap.

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
│   ├── arch/x86_64/      # HAL implementation, arch-specific code
│   ├── boot/             # Boot protocol parsing (BootInfo)
│   ├── mm/               # Memory management (PMM, VMM, kmalloc)
│   ├── proc/             # Process & thread management, scheduler
│   ├── fs/               # VFS and filesystem drivers
│   ├── drivers/          # Device drivers (char/, block/, net/)
│   ├── net/              # Network stack
│   ├── ipc/              # IPC mechanisms
│   ├── security/         # Permissions, capabilities
│   └── lib/              # Kernel utility library
├── libc/                 # User-space C library (musl port initially)
├── userland/             # User-space programs (init, shell, coreutils)
├── tools/                # Build tools, image creation scripts
├── tests/                # Host-side unit tests
├── docs/                 # ADRs, API docs
└── CMakeLists.txt
```

## Naming Conventions

- **Functions**: `snake_case` — e.g., `pmm_alloc_page()`, `hal_init()`
- **Types/Structs**: `PascalCase` or `snake_case_t` — e.g., `BootInfo`, `thread_t`
- **Constants/Macros**: `UPPER_SNAKE_CASE` — e.g., `PAGE_SIZE`, `GFP_KERNEL`
- **Files**: `snake_case.c` / `snake_case.h`
- **Subsystem prefixes**: Functions are prefixed with their subsystem — `pmm_`, `vmm_`, `hal_`, `vfs_`, `sched_`

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

## Phase Tracking

Current phase progress is tracked in `overview.md`. Each phase has concrete milestones. Follow the "Suggested Order of Attack" section for implementation sequence.
