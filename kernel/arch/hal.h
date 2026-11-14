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
#include "arch/x86_64/tsc.h"     /* TSC frequency (M1-04), clocksource (M1-06) */
#include "arch/x86_64/rtc.h"     /* CMOS real-time clock (M1-06) */
#include "arch/x86_64/power.h"   /* idle states, HWP, RAPL (M1-21) */
#include "arch/x86_64/debug_exit.h"   /* hal_debug_exit: QEMU test-mode exit */
#include "arch/x86_64/ipi.h"     /* halt the other CPUs on panic */
#include "arch/x86_64/lapic.h"   /* APIC mode for the boot report */
#include "arch/x86_64/irq_vector.h"   /* per-CPU device vectors, MSI format (M1-09) */
#include "arch/x86_64/tlb.h"     /* TLB shootdown (M1-07) */
#include "arch/x86_64/syscall.h"  /* syscall_register: subsystems add their handlers */
#include "arch/x86_64/usermode.h" /* jump_to_usermode, fork_return_to_user */
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
    cpu_sti_hlt();
}

/* --- Power (M1-21, arch/x86_64/power.h) --- */

/* Probe idle states, HWP and RAPL; turn HWP on every CPU with this EPP.
 * idle_override: POWER_IDLE_AUTO, _HLT or _C1 (arc.idle). Boot, after the
 * timer queues. */
static inline void hal_power_init(int idle_override, uint8_t epp) {
    power_hw_init(idle_override, epp);
}

/* What hal_power_init found (idle states, HWP, RAPL). */
static inline const PowerHwInfo *hal_power_info(void) {
    return power_hw_info();
}

/* The idle loop's sleep: interrupts off on entry, on at return; the state
 * is chosen for sleep_ns (time to the next timer, UINT64_MAX: none); a
 * write to wake's cache line may end it early. */
static inline void hal_idle_enter(uint64_t sleep_ns, const volatile void *wake) {
    power_idle_enter(sleep_ns, wake);
}

/* Idle residency (ns) and entries per state of CPU index cpu; -1 if offline. */
static inline int hal_idle_stats(uint32_t cpu, uint64_t ns[POWER_IDLE_MAX], uint64_t entries[POWER_IDLE_MAX]) {
    return power_idle_stats(cpu, ns, entries);
}

/* Energy preference every CPU requests (0 performance .. 255 power). */
static inline void hal_power_set_epp(uint8_t epp) {
    power_set_epp(epp);
}

/* Energy per RAPL domain in microjoules; returns the domains present. */
static inline uint32_t hal_power_energy(uint64_t uj[POWER_RAPL_DOMAINS]) {
    return power_energy_read(uj);
}

/* The safe MSR path, for the self-test: IA32_APIC_BASE (always there) read
 * with rdmsr_safe and plainly, and an MSR no CPU has read with rdmsr_safe
 * (a real CPU or KVM raises #GP: -EIO; QEMU's TCG reads unknown MSRs as 0). */
#define HAL_MSR_MISSING 0xDEADBEEFu
typedef struct {
    int present_rc;
    uint64_t present_safe, present_plain;
    int missing_rc;
    uint64_t missing_val;
} HalMsrCheck;
static inline void hal_msr_check(HalMsrCheck *c) {
    c->present_rc = rdmsr_safe(MSR_APIC_BASE, &c->present_safe);
    c->present_plain = rdmsr(MSR_APIC_BASE);
    c->missing_val = 0x5A5A5A5A5A5A5A5AULL;   /* the fixup must clear it */
    c->missing_rc = rdmsr_safe(HAL_MSR_MISSING, &c->missing_val);
}

/* Ask CPU index cpu to reschedule (an IPI; it runs sched_preempt). */
static inline void hal_send_resched(uint32_t cpu) {
    ipi_reschedule(cpu);
}

/* Let the APs, idling since hal_smp_init, enter sched_ap_start. */
static inline void hal_release_aps(void) {
    smp_release_aps();
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

/* 1 when a PCI function's firmware irq_line is where its INTx arrives (PIC
 * mode). In APIC mode the I/O APIC pin comes from ACPI _PRT (M4-11; usually
 * pins 16-23 on real chipsets), so irq_line is a guess that holds in QEMU. */
static inline int hal_irq_line_routing_known(void) {
    return !isr_apic_mode();
}

/* Let line through to the boot CPU for a device interrupt: the PIC unmasks it;
 * in APIC mode the I/O APIC already routes lines 0-15 (ioapic_route_isa, level
 * triggered where the MADT says so). Its EOI moves after the handler. */
static inline void hal_irq_line_enable(uint8_t line) {
    isr_set_late_eoi(line);   /* PCI INTx is level triggered: EOI once the device is acknowledged */
    if (!isr_apic_mode()) pic_unmask(line);
}

/* ==================================================================
 * Clocks and timers (M1-06)
 * ================================================================== */

/* Measure the TSC frequency (boot, BSP, after the CPU features are read).
 * The clock (hal_clock_ns) counts from here. */
static inline void hal_tsc_calibrate(void) {
    tsc_calibrate();
}

/* The TSC frequency, where it came from, and the boot sync check's result. */
static inline const TscInfo *hal_tsc_info(void) {
    return tsc_info();
}

/* Raw time-stamp counter (benchmarks; time goes through hal_clock_ns). */
static inline uint64_t hal_tsc_read(void) {
    return cpu_rdtsc();
}

/* TSC cycles to nanoseconds. */
static inline uint64_t hal_tsc_to_ns(uint64_t cycles) {
    return tsc_cycles_to_ns(cycles);
}

/* The clocksource: nanoseconds since boot (time/clock.c builds
 * CLOCK_MONOTONIC on it). Any CPU, any context. */
static inline uint64_t hal_clock_ns(void) {
    return tsc_clock_ns();
}

/* Nanoseconds to TSC cycles (time limits on boot-time polls). */
static inline uint64_t hal_clock_ns_to_cycles(uint64_t ns) {
    return tsc_ns_to_cycles(ns);
}

/* Finish the calibration once ACPI is read: the HPET at hpet_phys (0 =
 * none) when no better reference worked, or when force (test mode). */
static inline void hal_clock_calibrate_late(uint64_t hpet_phys, bool force) {
    tsc_calibrate_late(hpet_phys, force);
}

/* Check that the clocksource runs in step on every online CPU (ms each,
 * interrupts off). Returns the largest step back seen, in cycles. */
static inline uint64_t hal_clock_check_sync(uint32_t ms) {
    return tsc_check_sync(ms);
}

/* Clock events: one programmable interrupt per CPU (the local APIC timer). */
static inline int hal_clockevent_available(void) {
    return lapic_active();
}

/* BSP, once: fn becomes every CPU's clock-event handler (interrupt context,
 * after the EOI). TSC-deadline mode when the CPU has it, unless one_shot. */
static inline void hal_clockevent_init(void (*fn)(void), int one_shot) {
    lapic_timer_setup(fn, tsc_info()->deadline && !one_shot);
}

/* This CPU: put its timer in the chosen mode, stopped. */
static inline void hal_clockevent_cpu_init(void) {
    lapic_timer_cpu_init();
}

/* This CPU: one interrupt when hal_clock_ns() reaches ns (at once if it has;
 * may come early, then the caller reprograms). */
static inline void hal_clockevent_program(uint64_t ns) {
    lapic_timer_program(tsc_clock_to_tsc(ns));
}

/* This CPU: no clock-event interrupt until programmed again. */
static inline void hal_clockevent_stop(void) {
    lapic_timer_stop();
}

/* "TSC-deadline" or "LAPIC one-shot". */
static inline const char *hal_clockevent_mode(void) {
    return lapic_timer_mode();
}

/* Silence the legacy periodic timer (PIT channel 0): nothing ticks. */
static inline void hal_legacy_timer_stop(void) {
    pit_stop();
}

/* This CPU's timer queue (time/timer.c), NULL before it is set. */
static inline void *hal_this_cpu_timers(void) {
    return this_cpu()->timers;
}

/* Timer queue of CPU index cpu, or NULL. */
static inline void *hal_cpu_timers(uint32_t cpu) {
    PerCpu *c = percpu_get(cpu);
    return c != NULL ? c->timers : NULL;
}

/* Attach a timer queue to CPU index cpu (boot). */
static inline void hal_cpu_set_timers(uint32_t cpu, void *timers) {
    PerCpu *c = percpu_get(cpu);
    if (c != NULL) c->timers = timers;
}

/* Run fn(arg) on every online CPU at once, this one included (interrupts
 * off on each), and wait up to max_cycles. Returns how many other CPUs ran
 * it. Boot only, from the BSP. */
static inline uint32_t hal_run_on_all_cpus(void (*fn)(void *), void *arg, uint64_t max_cycles) {
    return ipi_call_all(fn, arg, max_cycles);
}

/* Read the battery-backed real-time clock (UTC). century_reg from the ACPI
 * FADT (0 = none). Returns 0, or -1 when it gives no valid date within
 * max_cycles TSC cycles. */
static inline int hal_rtc_read(uint8_t century_reg, uint64_t max_cycles, RtcTime *out) {
    return rtc_read(century_reg, max_cycles, out);
}

/* Per-CPU interrupt counts for /proc/interrupts */
#define HAL_IRQ_STAT_TIMER   PERCPU_STAT_TIMER     /* clock-event interrupts */
#define HAL_IRQ_STAT_RESCHED PERCPU_STAT_RESCHED   /* reschedule IPIs */
#define HAL_IRQ_STAT_TLB     PERCPU_STAT_TLB       /* TLB shootdown IPIs */
#define HAL_IRQ_STAT_CALL    PERCPU_STAT_CALL      /* function-call IPIs */
#define HAL_IRQ_STATS        PERCPU_STATS

/* Count `which` (HAL_IRQ_STAT_*) of CPU index cpu, 0 if it is not online. */
static inline uint64_t hal_cpu_irq_stat(uint32_t cpu, int which) {
    PerCpu *c = percpu_get(cpu);
    if (c == NULL || !c->online || which < 0 || which >= PERCPU_STATS) return 0;
    return __atomic_load_n(&c->irq_stat[which], __ATOMIC_RELAXED);
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

/* uaccess.asm: copy n bytes between kernel and user memory; a user page
 * that can't be faulted in ends the copy. Returns the bytes not copied. */
size_t arch_copy_user(void *dst, const void *src, size_t n);

/* Copy with the exception-table fixup (mm/uaccess.c is the only caller).
 * Returns the bytes not copied (0 = all). */
static inline size_t hal_copy_user(void *dst, const void *src, size_t n) {
    return arch_copy_user(dst, src, n);
}

/* Flush [start, end) of address space pgd here (if loaded) and on every CPU
 * in cpus, and wait for them (tlb.h). No spinlock may be held. Returns how
 * many other CPUs flushed. */
static inline uint32_t hal_tlb_flush(const CpuMask *cpus, uint64_t pgd, uint64_t start, uint64_t end) {
    return tlb_shootdown(cpus, pgd, start, end);
}

/* Flush a kernel mapping range on every CPU (kernel mappings are in every
 * address space). No spinlock may be held. */
static inline void hal_tlb_flush_kernel(uint64_t start, uint64_t end) {
    (void)tlb_shootdown(NULL, 0, start, end);
}

/* The user address space this CPU has loaded (mm.c's bookkeeping), or NULL. */
static inline void *hal_this_cpu_mm(void) {
    return this_cpu()->mm;
}

/* Record the address space this CPU has loaded. Interrupts off. */
static inline void hal_set_this_cpu_mm(void *mm) {
    this_cpu()->mm = mm;
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
