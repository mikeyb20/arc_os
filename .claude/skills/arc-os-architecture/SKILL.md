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
6. Syscalls follow the **Linux x86_64 ABI** (ADR-0001). Never invent a number.
7. Binding decisions live in `docs/decisions/` (ADR-0001…0017). Code that contradicts an ADR is a bug.

## Roadmap Status

The roadmap is `docs/plans/knowledge-os.md` (milestones M0–M5); the build order, block specs and status are in `docs/plans/work-blocks.md` and `docs/plans/blocks/`. `overview.md` phases 0–12 are history and are complete or mostly complete.

**Current state (2026-11-07)**: phases 0–12 done; M0 (lock-in decisions) in progress. Known gaps vs the plan: no FPU/XSAVE save, no mmap/page cache, no user threads, VirtIO-only drivers, read-only FAT32, arc_os-specific syscall numbers (being migrated).

## Reference Files

Detailed reference documents in `reference/`:
- `hal-interface.md` — HAL function signatures and contracts
- `memory-layout.md` — Kernel address space layout, page table structure
- `boot-protocol.md` — BootInfo struct definition, bootloader contract
- `syscall-table.md` — Linux ABI target, migration map, per-syscall status
- `build-system.md` — How the build works, targets, toolchain details
