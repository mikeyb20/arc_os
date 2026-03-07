# HAL Interface Reference

## Current State

> **Note**: A unified `kernel/arch/hal.h` does not exist yet. HAL consolidation (Chunk 1.10) is deferred. The kernel currently calls architecture-specific functions directly. This document describes the de-facto HAL — the set of arch-specific interfaces used by portable kernel code.

## Initialization (direct calls, no HAL wrapper)

```c
/* kernel/arch/x86_64/gdt.h */
void gdt_init(void);                    /* GDT + TSS setup and load */
void gdt_set_kernel_stack(uint64_t rsp0); /* Set TSS.RSP0 for ring transitions */

/* kernel/arch/x86_64/idt.h */
void idt_init(void);                    /* IDT setup with ISR stubs */

/* kernel/arch/x86_64/pic.h */
void pic_init(void);                    /* PIC remap: IRQ 0-7→32-39, 8-15→40-47 */

/* kernel/arch/x86_64/pit.h */
void pit_init(uint32_t frequency);      /* PIT channel 0 at given Hz */

/* kernel/arch/x86_64/serial.h */
void serial_init(void);                 /* COM1 (0x3F8) init */
```

## Interrupt Control (inline asm, no HAL wrapper)

```c
/* Used directly via inline asm — no hal_enable_interrupts() wrapper exists */
asm volatile ("sti");   /* Enable interrupts */
asm volatile ("cli");   /* Disable interrupts */
```

## Paging / Virtual Memory

```c
/* kernel/arch/x86_64/paging.h — inline functions */
uint64_t paging_read_cr3(void);         /* Read current PML4 physical address */
void paging_write_cr3(uint64_t cr3);    /* Switch address space */
void paging_invlpg(uint64_t vaddr);     /* Invalidate TLB entry */

/* kernel/mm/vmm.h — portable VMM interface (calls paging.h internally) */
void vmm_map_page(uint64_t virt, uint64_t phys, uint32_t flags);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_phys(uint64_t virt);
```

## Context Switching

```c
/* kernel/arch/x86_64/context_switch.asm — called directly, no hal_context_switch() */
void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
/* Saves callee-saved regs (r15-r12, rbx, rbp) + RSP; restores from new stack */
```

## GDT Layout

| Index | Selector | Segment | Notes |
|-------|----------|---------|-------|
| 0 | 0x00 | Null | Required by x86 |
| 1 | 0x08 | Kernel Code | 64-bit, DPL 0 |
| 2 | 0x10 | Kernel Data | DPL 0 |
| 3 | 0x18 | User Data | DPL 3, must precede user code for SYSRET |
| 4 | 0x20 | User Code | 64-bit, DPL 3 |
| 5 | 0x28 | TSS | 16-byte descriptor (two GDT slots) |

## IST (Interrupt Stack Table)

- **IST1**: Double fault handler — uses a dedicated 4KB static stack to ensure double faults are always catchable even if the kernel stack is corrupted
- IST2-IST7: Not currently used (NMI handler does not use a dedicated IST entry)

## Page Flags (VMM portable flags)

```c
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)
```

These are mapped to x86_64 PTE flags internally by `vmm_map_page()`.
