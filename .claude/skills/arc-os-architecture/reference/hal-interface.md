# HAL Interface Reference

The headers are the reference; this page only says where to look (ADR-0020: docs don't restate code).

- **`kernel/arch/hal.h`**: the one header code outside `kernel/arch/` includes for hardware access: CPU and
  interrupt control (`hal_enable_interrupts`, `hal_save_irq_state`, ...), IRQ routing (`hal_irq_*`), the timer
  (`hal_timer_*`), address-space switching (`hal_switch_address_space`, `hal_invalidate_page`), the TSS kernel
  stack, serial, memory fences. It also includes `fpu.h` (user FPU state, FS base) and `cpuid.h`.
- **Rule:** new code outside `kernel/arch/` never includes `arch/x86_64/...` directly. The 44 old direct includes
  are frozen by the `arch_includes_outside_arch` budget and move behind `hal.h` as M1 blocks rewrite their files
  (M1-03, M1-05, M1-06, M1-07, M1-09). See `docs/decisions/0020-code-hygiene-ratchets.md`.
- **Arch internals** (`kernel/arch/x86_64/`): GDT and selectors (`gdt.h`, selectors must keep user data before
  user code for SYSRET), IDT and ISR stubs (`idt.h`, `isr.h`), double-fault IST stack (`gdt.c`), paging
  primitives (`paging.h`), SYSCALL entry (`syscall_entry.asm`, `syscall.h`), context switch
  (`context_switch.asm`, declared in `kernel/proc/thread.h`).
- **Portable VM interface:** `kernel/mm/vmm.h` (`VMM_FLAG_*`, mapping, user address-space helpers).
