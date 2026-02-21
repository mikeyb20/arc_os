# Memory Layout Reference

## Physical Memory Map

Physical memory layout is discovered at boot from the bootloader-provided memory map.

Typical regions:
- `0x00000000 - 0x000FFFFF` — Low memory (legacy, partially reserved, BIOS data)
- `0x00100000 - varies` — Usable RAM (kernel loaded here)
- Various reserved regions — ACPI, MMIO, framebuffer

The PMM tracks which physical frames are free/allocated using a bitmap or buddy allocator.

## x86_64 Virtual Address Space Layout

Higher-half kernel design. The kernel is mapped in the upper half of the 64-bit address space.

```
0x0000000000000000 ┌──────────────────────────────────┐
                   │       User Space (per-process)     │
                   │  Code, data, heap, stack, mmap     │
                   │  Grows up to canonical boundary     │
0x00007FFFFFFFFFFF ├──────────────────────────────────┤
                   │       Non-canonical hole            │
                   │  (not addressable on x86_64)        │
0xFFFF800000000000 ├──────────────────────────────────┤
                   │  Physical Memory Direct Map         │
                   │  All physical RAM mapped here       │
                   │  phys_to_virt(p) = p + 0xFFFF800000000000
0xFFFFFFFF80000000 ├──────────────────────────────────┤
                   │  Kernel Image (.text, .rodata,      │
                   │  .data, .bss)                       │
                   │  Loaded by bootloader here          │
0xFFFFFFFFC0000000 ├──────────────────────────────────┤
                   │  Kernel Heap                        │
                   │  Grows upward, managed by kmalloc   │
0xFFFFFFFFE0000000 ├──────────────────────────────────┤
                   │  Per-CPU Data (Phase 12)            │
                   │  Accessed via GS segment base       │
0xFFFFFFFFFF000000 ├──────────────────────────────────┤
                   │  Device MMIO Mappings               │
                   │  Memory-mapped I/O for devices      │
0xFFFFFFFFFFFFFFFF └──────────────────────────────────┘
```

## Page Table Structure (x86_64 4-Level Paging)

```
PML4 (512 entries) → PDPT (512 entries) → PD (512 entries) → PT (512 entries) → 4KB Page
```

- Each level uses 9 bits of the virtual address
- PML4 index: bits 47:39
- PDPT index: bits 38:30
- PD index: bits 29:21
- PT index: bits 20:12
- Page offset: bits 11:0

Large pages: 2MB (PD entry, no PT) or 1GB (PDPT entry, no PD/PT).

## Kernel Stack

- Each kernel thread gets its own kernel stack
- Stack size: 16KB (4 pages) initially
- Guard page below each stack (unmapped, triggers page fault on overflow)
- IST (Interrupt Stack Table) entries for double fault and NMI handlers

## Constants

```c
#define PAGE_SIZE          4096
#define KERNEL_VIRT_BASE   0xFFFFFFFF80000000ULL
#define PHYS_MAP_BASE      0xFFFF800000000000ULL
#define KERNEL_HEAP_BASE   0xFFFFFFFFC0000000ULL
#define KERNEL_STACK_SIZE  (4 * PAGE_SIZE)
```
