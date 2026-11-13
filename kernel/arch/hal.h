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
#include "arch/x86_64/tsc.h"     /* TSC frequency (M1-04) */
#include "arch/x86_64/debug_exit.h"   /* hal_debug_exit: QEMU test-mode exit */
#include "arch/x86_64/ipi.h"     /* halt the other CPUs on panic */
#include "arch/x86_64/lapic.h"   /* APIC mode for the boot report */
#include "arch/x86_64/irq_vector.h"   /* per-CPU device vectors, MSI format (M1-09) */
#include "arch/hal_lock.h"        /* irq_save/irq_restore, hal_cpu_relax, spinlock depth */

/* ==================================================================
 * CPU Initialization & Control
 * ================================================================== */

/* First thing at boot: point GS at the boot CPU's per-CPU block, so spinlocks
 * can count their depth (hal_lock.h) before hal_cpu_init runs. */
static inline void hal_cpu_early_init(void) {
    percpu_early_init();
}

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

/* Interrupts off, halt; returns only on an NMI (callers loop). */
static inline void hal_halt_forever(void) {
    __asm__ volatile("cli; hlt");
}

/* Index of the CPU this runs on (0 = BSP); safe from the first line of boot. */
static inline uint32_t hal_cpu_index(void) {
    return percpu_current_index();
}

/* Halt every other CPU (panic). Does nothing before the APs are started. */
static inline void hal_stop_other_cpus(void) {
    if (smp_active()) ipi_halt_all();
}

/* Registers and backtrace for a panic raised by code (lib/panic.h). */
static inline void hal_panic_capture(PanicInfo *pi, void *frame_address) {
    arch_panic_capture(pi, frame_address);
}

/* Halt the CPU until the next interrupt. */
static inline void hal_halt(void) {
    __asm__ volatile ("hlt");
}

/* ---- CPU description and cross-CPU calls (boot report, self-test; M1-04) ---- */

/* Brand string ("" when the CPU has none). */
static inline void hal_cpu_brand(char *buf, size_t size) {
    cpu_brand(buf, size);
}

/* Vendor string ("GenuineIntel"), family, model, stepping. */
static inline const char *hal_cpu_vendor(void) {
    return cpu_features()->vendor;
}

static inline void hal_cpu_signature(uint32_t *family, uint32_t *model, uint32_t *stepping) {
    cpu_signature(family, model, stepping);
}

/* Notable features present, space separated. */
static inline void hal_cpu_feature_names(char *buf, size_t size) {
    cpu_feature_names(buf, size);
}

/* APIC ID and core type name ("P", "E", "-") of online CPU index cpu.
 * Returns 0, or -1 when that CPU is not online. */
static inline int hal_cpu_info(uint32_t cpu, uint32_t *apic_id, const char **core) {
    uint8_t type;
    if (cpu_describe(cpu, apic_id, &type) != 0) return -1;
    *core = cpu_core_type_name(type);
    return 0;
}

/* Highest CPU index hal_cpu_info accepts, plus one. */
#define HAL_MAX_CPUS CPU_MAX

/* CPUs online (BSP + APs that came up). */
static inline uint32_t hal_cpu_count(void) {
    return smp_cpu_count();
}

/* Interrupt controller mode: "x2APIC", "xAPIC" (once hal_smp_init ran). */
static inline const char *hal_apic_mode(void) {
    return lapic_x2apic() ? "x2APIC" : "xAPIC";
}

/* Run fn(arg) on every other online CPU and wait up to max_cycles TSC cycles.
 * Returns how many ran it. Boot diagnostics only, from the BSP. */
static inline uint32_t hal_run_on_other_cpus(void (*fn)(void *), void *arg, uint64_t max_cycles) {
    return ipi_call_others(fn, arg, max_cycles);
}

/* ---- Scheduler support (M1-05) ---- */

/* This CPU's run queue (proc/sched.c), NULL before the scheduler set it. */
static inline void *hal_this_cpu_sched(void) {
    return this_cpu()->sched;
}

/* Run queue of CPU index cpu, or NULL. */
static inline void *hal_cpu_sched(uint32_t cpu) {
    PerCpu *c = percpu_get(cpu);
    return c != NULL ? c->sched : NULL;
}

/* Attach a run queue to CPU index cpu (boot, before that CPU schedules). */
static inline void hal_cpu_set_sched(uint32_t cpu, void *rq) {
    PerCpu *c = percpu_get(cpu);
    if (c != NULL) c->sched = rq;
}

/* 1 if CPU index cpu is online. */
static inline int hal_cpu_online(uint32_t cpu) {
    PerCpu *c = percpu_get(cpu);
    return c != NULL && c->online;
}

/* Core kinds for hal_cpu_core_kind */
#define HAL_CORE_OTHER 0   /* not a hybrid CPU, or unknown */
#define HAL_CORE_P     1
#define HAL_CORE_E     2

/* Core kind of online CPU index cpu (HAL_CORE_*), or -1 if it is not online. */
static inline int hal_cpu_core_kind(uint32_t cpu) {
    uint32_t apic;
    uint8_t type;
    if (cpu_describe(cpu, &apic, &type) != 0) return -1;
    return type == CPU_CORE_CORE ? HAL_CORE_P : (type == CPU_CORE_ATOM ? HAL_CORE_E : HAL_CORE_OTHER);
}

/* Idle: enable interrupts and halt until the next one, in one step (no
 * wakeup can slip in between). Returns with interrupts on. */
static inline void hal_idle_wait(void) {
    __asm__ volatile("sti; hlt" ::: "memory");
}

/* Ask CPU index cpu to reschedule (an IPI; it runs sched_preempt). */
static inline void hal_send_resched(uint32_t cpu) {
    ipi_reschedule(cpu);
}

/* Let the APs, idling since hal_smp_init, enter sched_ap_start. */
static inline void hal_release_aps(void) {
    smp_release_aps();
}

/* Slice timer: this CPU's local APIC timer (TSC-deadline mode comes with
 * M1-06). hal_slice_timer_init measures it once on the BSP and sets fn as the
 * handler on every CPU; start/stop act on the calling CPU. */
static inline void hal_slice_timer_init(void (*fn)(void)) {
    lapic_timer_setup(fn);
}
static inline void hal_slice_timer_start(uint64_t period_ns) {
    lapic_timer_periodic(period_ns);
}
static inline void hal_slice_timer_stop(void) {
    lapic_timer_stop();
}

/* Self-test only (arc.selftest=1, no user thread exists): AVX2 sum of buf on
 * this CPU after checking XCR0; see fpu_avx2_selftest. */
static inline int hal_avx2_selftest(const uint32_t *buf, uint64_t count, uint32_t *sum, uint64_t *xcr0) {
    return fpu_avx2_selftest(buf, count, sum, xcr0);
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

/* ---- Device interrupts: per-CPU vectors, MSI, legacy lines (M1-09) ---- */

/* Set the function every bound device vector calls with its cookie
 * (interrupts off; the EOI follows it). */
static inline void hal_irq_set_entry(void (*fn)(void *cookie)) {
    irq_vector_set_entry(fn);
}

/* Take a free device vector on CPU index cpu, bound to cookie. Returns the
 * vector, or -EINVAL, -ENOMEM, -ENOSPC. Thread context. */
static inline int hal_irq_vector_alloc(uint32_t cpu, void *cookie) {
    return irq_vector_alloc(cpu, cookie);
}

/* Give a device vector back. */
static inline void hal_irq_vector_free(uint32_t cpu, int vector) {
    irq_vector_free(cpu, vector);
}

/* MSI address/data for vector on CPU index cpu: 0, -EINVAL or -ERANGE (APIC
 * ID above 255: needs interrupt remapping). */
static inline int hal_irq_msi_message(uint32_t cpu, int vector, uint64_t *addr, uint32_t *data) {
    return irq_msi_compose(cpu, vector, addr, data);
}

/* 1 when message-signalled interrupts can be delivered (local APIC on). */
static inline int hal_irq_msi_available(void) {
    return lapic_active();
}

/* 1 if CPU index cpu is online and can be an MSI target (APIC ID < 256). */
static inline int hal_irq_msi_target_ok(uint32_t cpu) {
    PerCpu *c = percpu_get(cpu);
    return c != NULL && c->online && c->apic_id <= 0xFF;
}

/* Device vectors that arrived with nothing bound to them. */
static inline uint64_t hal_irq_spurious_count(void) {
    return irq_vector_spurious();
}

/* Legacy interrupt lines (ISA IRQs, and PCI INTx as the firmware routed it):
 * line n arrives on vector HAL_IRQ_LINE_BASE + n of the boot CPU. */
#define HAL_IRQ_LINES     16
#define HAL_IRQ_LINE_BASE IRQ_BASE

/* The handler on legacy line's vector, or NULL (a line the kernel itself uses
 * for the PIT, the keyboard, ... is not free for a PCI device). */
static inline isr_handler_t hal_irq_line_handler(uint8_t line) {
    return isr_get_handler(HAL_IRQ_LINE_BASE + line);
}

/* Let line through to the boot CPU for a device interrupt: the PIC unmasks it;
 * in APIC mode the I/O APIC already routes lines 0-15 (ioapic_route_isa, level
 * triggered where the MADT says so). Its EOI moves after the handler. */
static inline void hal_irq_line_enable(uint8_t line) {
    isr_set_late_eoi(line);   /* PCI INTx is level triggered: EOI once the device is acknowledged */
    if (!isr_apic_mode()) pic_unmask(line);
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

/* Measure the TSC frequency (boot, BSP, after the CPU features are read). */
static inline void hal_tsc_calibrate(void) {
    tsc_calibrate();
}

/* The TSC frequency and where it came from. */
static inline const TscInfo *hal_tsc_info(void) {
    return tsc_info();
}

/* Raw time-stamp counter. */
static inline uint64_t hal_tsc_read(void) {
    return cpu_rdtsc();
}

/* TSC cycles to nanoseconds (0 when the frequency is unknown). */
static inline uint64_t hal_tsc_to_ns(uint64_t cycles) {
    return tsc_cycles_to_ns(cycles);
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

/* Kernel virtual window for device memory (MMIO helper, M1-09): 512 MiB at
 * the old heap window, above the kernel image, below the top 512 MiB. Pages
 * mapped there are uncached and never unmapped. */
#define HAL_MMIO_BASE 0xFFFFFFFFC0000000ULL
#define HAL_MMIO_SIZE (512ULL << 20)

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
