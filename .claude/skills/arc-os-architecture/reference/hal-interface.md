# HAL Interface Reference

The Hardware Abstraction Layer isolates all architecture-specific code. Everything above the HAL is portable across architectures.

## HAL Header: `kernel/arch/hal.h`

This is the portable interface. Each architecture implements these functions in `kernel/arch/<arch>/`.

## Core Initialization

```c
/* Initialize architecture: GDT, IDT, segment registers, CPU mode setup */
void hal_init(void);

/* Early console for pre-MM output (serial port) */
void hal_early_putchar(char c);
```

## Interrupt Control

```c
/* Enable/disable interrupts globally */
void hal_enable_interrupts(void);
void hal_disable_interrupts(void);

/* Save and restore interrupt state (for nested critical sections) */
uint64_t hal_save_interrupt_state(void);
void hal_restore_interrupt_state(uint64_t state);
```

## Timer

```c
/* Read current tick count (monotonic) */
uint64_t hal_read_timer(void);

/* Set a one-shot or periodic timer callback */
void hal_set_timer(uint64_t interval_ms, void (*callback)(void));
```

## Paging / Virtual Memory

```c
/* Map a virtual page to a physical frame with given flags */
int hal_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Unmap a virtual page */
void hal_unmap_page(uintptr_t virt);

/* Invalidate a TLB entry for a given virtual address */
void hal_invalidate_page(uintptr_t virt);

/* Switch to a different address space (page table root) */
void hal_switch_address_space(uintptr_t page_table_phys);
```

## Context Switching

```c
/* Switch from old thread context to new thread context */
void hal_context_switch(thread_t *old, thread_t *new);
```

## SMP (Phase 12)

```c
/* Get current CPU ID */
uint32_t hal_get_cpu_id(void);

/* Send inter-processor interrupt */
void hal_send_ipi(uint32_t target_cpu, uint8_t vector);
```

## Page Flags

```c
#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITABLE   (1 << 1)
#define PAGE_USER       (1 << 2)
#define PAGE_NO_EXECUTE (1ULL << 63)  /* x86_64 NX bit */
```

## x86_64 Implementation Notes

- GDT with kernel code/data and user code/data segments, TSS
- IDT with 256 entries (exceptions 0-31, IRQs 32+, syscall 128)
- Long mode verification on entry
- SSE enabled (required by System V ABI for x86_64)
- CR3 holds PML4 physical address for address space switching
- Context switch saves/restores: RBX, RBP, R12-R15, RSP, RIP (callee-saved)
