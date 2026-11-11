#ifndef ARCHOS_ARCH_HAL_H
#define ARCHOS_ARCH_HAL_H

/* Hardware Abstraction Layer — unified facade over architecture-specific code.
 *
 * This header provides a single include for generic kernel code that needs
 * to interact with the hardware. It groups architecture-specific operations
 * by function rather than by hardware unit. All functions delegate to the
 * underlying x86_64 implementations.
 *
 * Code outside kernel/arch/ includes this header, never the arch/x86_64/
 * headers directly (ADR-0020); old direct includes move as blocks rewrite them. */

#include <stdint.h>
#include <stdbool.h>

/* ---- Underlying arch headers ---- */
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/serial.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/fpu.h"     /* fpu_*, arch_thread_switch, arch_set_fs_base */
#include "arch/x86_64/cpuid.h"   /* cpu_hwcap, cpu_random_fill */
#include "arch/x86_64/cpu.h"     /* CPU features, floor, core types */
#include "arch/x86_64/percpu.h"  /* this_cpu(), per-CPU kernel stacks */
#include "arch/x86_64/smp.h"     /* APIC switch-over, AP start */

/* ==================================================================
 * CPU Initialization & Control
 * ================================================================== */

/* Set up the boot CPU: its per-CPU block (GDT, TSS, IST stacks, GS base),
 * then the shared IDT. Must run before anything calls thread_current(). */
static inline void hal_cpu_init(void) {
    percpu_init_bsp();
    idt_init();
}

/* Switch to the APIC interrupt controllers and start the other CPUs (they
 * idle until M1-05). Needs the page allocator; interrupts still off. Returns
 * the number of CPUs online. */
static inline uint32_t hal_smp_init(const BootInfo *info, const AcpiInfo *acpi) {
    return smp_init(info, acpi);
}

/* The thread running on this CPU (kept in the per-CPU block). */
static inline struct Thread *hal_current_thread(void) {
    return this_cpu_thread();
}

/* Record the thread now running on this CPU. */
static inline void hal_set_current_thread(struct Thread *t) {
    this_cpu()->current_thread = t;
}

/* Enable hardware interrupts. */
static inline void hal_enable_interrupts(void) {
    __asm__ volatile ("sti");
}

/* Disable hardware interrupts. */
static inline void hal_disable_interrupts(void) {
    __asm__ volatile ("cli");
}

/* Halt the CPU until the next interrupt. */
static inline void hal_halt(void) {
    __asm__ volatile ("hlt");
}

/* Save interrupt state and return it (for save/restore pattern). */
static inline uint64_t hal_save_irq_state(void) {
    uint64_t flags;
    __asm__ volatile ("pushf; pop %0" : "=r"(flags));
    return flags;
}

/* Restore previously saved interrupt state. */
static inline void hal_restore_irq_state(uint64_t flags) {
    __asm__ volatile ("push %0; popf" : : "r"(flags) : "memory", "cc");
}

/* ==================================================================
 * Interrupt & IRQ Management
 * ================================================================== */

/* Initialize the interrupt controller and remap IRQs. */
static inline void hal_irq_init(void) {
    pic_init();
}

/* Register a handler for an interrupt vector. */
static inline void hal_irq_register(int vector, isr_handler_t handler) {
    isr_register_handler(vector, handler);
}

/* Send end-of-interrupt for the given IRQ (0-15). */
static inline void hal_irq_eoi(uint8_t irq) {
    pic_send_eoi(irq);
}

/* Enable (unmask) a specific IRQ line. */
static inline void hal_irq_unmask(uint8_t irq) {
    pic_unmask(irq);
}

/* Disable (mask) a specific IRQ line. */
static inline void hal_irq_mask(uint8_t irq) {
    pic_mask(irq);
}

/* Check if an IRQ is spurious. */
static inline bool hal_irq_is_spurious(uint8_t irq) {
    return pic_is_spurious(irq);
}

/* ==================================================================
 * Timer
 * ================================================================== */

/* Initialize periodic timer at the given frequency in Hz. */
static inline void hal_timer_init(uint32_t freq_hz) {
    pit_init(freq_hz);
}

/* Get total tick count since timer initialization. */
static inline uint64_t hal_timer_ticks(void) {
    return pit_get_ticks();
}

/* Get approximate uptime in milliseconds. */
static inline uint64_t hal_timer_uptime_ms(void) {
    return pit_get_uptime_ms();
}

/* ==================================================================
 * Paging / Address Space
 * ================================================================== */

/* Switch to a new page table (PML4 physical address). Flushes TLB. */
static inline void hal_switch_address_space(uint64_t pml4_phys) {
    paging_write_cr3(pml4_phys);
}

/* Get current page table physical address. */
static inline uint64_t hal_get_address_space(void) {
    return paging_read_cr3();
}

/* Invalidate TLB entry for a single virtual address. */
static inline void hal_invalidate_page(uint64_t vaddr) {
    paging_invlpg(vaddr);
}

/* ==================================================================
 * Task State (kernel stack for ring transitions)
 * ================================================================== */

/* Set the kernel stack used on ring 3→0 transitions on this CPU: interrupts
 * (TSS.RSP0) and SYSCALL alike. Call on every switch to a thread. */
static inline void hal_set_kernel_stack(uint64_t rsp0) {
    percpu_set_kernel_stack(rsp0);
}

/* ==================================================================
 * Port I/O
 * ================================================================== */

/* Port I/O functions are already provided as static inlines by io.h:
 *   outb, inb, outw, inw, outl, inl, io_wait
 * They are available through this header via the include above. */

/* ==================================================================
 * Serial Console
 * ================================================================== */

/* Initialize serial port for debug output. */
static inline void hal_serial_init(void) {
    serial_init();
}

/* Write a character to the serial debug console. */
static inline void hal_serial_putchar(char c) {
    serial_putchar(c);
}

/* Write a string to the serial debug console. */
static inline void hal_serial_puts(const char *s) {
    serial_puts(s);
}

/* ==================================================================
 * Memory Barriers
 * ================================================================== */

/* Full memory fence (serializes loads and stores). */
static inline void hal_memory_fence(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

/* Load fence (serializes loads). */
static inline void hal_load_fence(void) {
    __asm__ volatile ("lfence" ::: "memory");
}

/* Store fence (serializes stores). */
static inline void hal_store_fence(void) {
    __asm__ volatile ("sfence" ::: "memory");
}

#endif /* ARCHOS_ARCH_HAL_H */
