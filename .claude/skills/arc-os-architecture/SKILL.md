# arc-os-architecture

Reference skill providing deep context about the arc_os kernel architecture, layer boundaries, and interfaces.

## When to Use

Invoke this skill when you need to understand:
- How the kernel layers are structured and what interfaces they expose
- The HAL boundary and what's arch-specific vs. portable
- Memory layout (physical and virtual address spaces)
- Boot protocol and BootInfo contract
- Syscall table and calling conventions
- Build system details and targets

## Architecture Overview

arc_os uses the **Abstraction Boundary Model**: the OS is a stack of replaceable layers, each exposing a stable interface. Implementations behind the interface can be swapped freely.

```
┌─────────────────────────────────────────────────┐
│              User-Space Applications            │
├─────────────────────────────────────────────────┤
│              System Call Interface               │
├─────────────────────────────────────────────────┤
│   VFS   │  Scheduler  │  IPC  │  Net Stack      │
├─────────────────────────────────────────────────┤
│        Memory Manager  │  Driver Framework       │
├─────────────────────────────────────────────────┤
│     Hardware Abstraction Layer (HAL)             │
├─────────────────────────────────────────────────┤
│     Bootloader Protocol Interface                │
├─────────────────────────────────────────────────┤
│              Hardware / QEMU                     │
└─────────────────────────────────────────────────┘
```

## Key Rules

1. Layers only call **downward** through defined interfaces
2. Never reach into another layer's internals
3. HAL isolates all arch-specific code — everything above is portable
4. Kernel never knows which bootloader was used — reads `BootInfo`
5. User programs link against libc, never call syscalls directly

## Phase Status

Track implementation progress in `overview.md` → "Suggested Order of Attack" section.

**Current focus**: Phase 0 (tooling, build system, environment setup)

## Reference Files

Detailed reference documents in `reference/`:
- `hal-interface.md` — HAL function signatures and contracts
- `memory-layout.md` — Kernel address space layout, page table structure
- `boot-protocol.md` — BootInfo struct definition, bootloader contract
- `syscall-table.md` — Syscall numbers, signatures, conventions
- `build-system.md` — How the build works, targets, toolchain details
