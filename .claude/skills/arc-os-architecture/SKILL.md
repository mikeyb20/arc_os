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
7. Binding decisions live in `docs/decisions/` (ADR-0001…0018). Code that contradicts an ADR is a bug.

## Roadmap Status

The roadmap is `docs/plans/knowledge-os.md` (milestones M0–M5); the build order, block specs and status are in `docs/plans/work-blocks.md` and `docs/plans/blocks/`. `overview.md` phases 0–12 are history and are complete or mostly complete.

**Current state**: see `docs/plans/work-blocks.md` (block status) and `docs/plans/knowledge-os.md` §8 (milestones). This skill doesn't track it, so it can't go stale.

## Reference Files

Detailed reference documents in `reference/`:
- `hal-interface.md` — where the HAL lives (`kernel/arch/hal.h`) and the include rule
- `memory-layout.md` — which header defines each address-space constant
- `boot-protocol.md` — BootInfo struct definition, bootloader contract
- `syscall-table.md` — Linux ABI target, migration map, per-syscall status
- `build-system.md` — How the build works, targets, toolchain details
