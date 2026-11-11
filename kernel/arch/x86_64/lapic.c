/* arc_os — Local APIC access layer (M1-03)
 *
 * Owns: the APIC mode (xAPIC MMIO or x2APIC MSRs), register access, EOI, IPIs.
 * Invariants: the mode is chosen once on the BSP before any AP starts and never
 * changes; every CPU runs in that mode (an AP left in xAPIC mode by the
 * bootloader is switched to x2APIC; xAPIC -> x2APIC is the only legal direction
 * without a reset). In xAPIC mode lapic_mmio is the HHDM address of the MMIO page.
 * Locks: none. Register access is per CPU; an xAPIC IPI is two register writes,
 * so lapic_send_ipi keeps interrupts off across them. */

#include "arch/x86_64/lapic.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/pit.h"
#include "mm/vmm.h"
#include "proc/spinlock.h"
#include "lib/kprintf.h"
#include <stddef.h>

#define CPUID1_ECX_X2APIC (1u << 21)

static volatile uint32_t *lapic_mmio;   /* xAPIC mode only */
static int x2apic_mode;

int lapic_x2apic(void) {
    return x2apic_mode;
}

uint32_t lapic_read(uint32_t reg) {
    if (x2apic_mode) return (uint32_t)rdmsr(MSR_X2APIC_FIRST + (reg >> 4));
    return lapic_mmio[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t val) {
    if (x2apic_mode) {
        wrmsr(MSR_X2APIC_FIRST + (reg >> 4), val);
        return;
    }
    lapic_mmio[reg / 4] = val;
}

/* Enable the APIC on this CPU in the chosen mode, then program it. */
static void lapic_enable_this_cpu(void) {
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (x2apic_mode) {
        if (!(base & APIC_BASE_EXTD)) {
            /* xAPIC -> x2APIC: EN must stay set; both bits in one write */
            wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN | APIC_BASE_EXTD);
        }
    } else if (!(base & APIC_BASE_EN)) {
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN);
    }

    /* Enable LAPIC via Spurious Interrupt Vector Register */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);

    /* Clear Error Status Register (write twice as per spec; x2APIC needs 0) */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* Set Task Priority Register to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* Send EOI to clear any pending interrupts */
    lapic_eoi();
}

void lapic_init_bsp(uint64_t mmio_phys) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint64_t base = rdmsr(MSR_APIC_BASE);

    x2apic_mode = (ecx & CPUID1_ECX_X2APIC) || (base & APIC_BASE_EXTD);
    if (!x2apic_mode) {
        if (mmio_phys == 0) mmio_phys = base & APIC_BASE_ADDR_MASK;
        lapic_mmio = (volatile uint32_t *)(mmio_phys + vmm_get_hhdm_offset());
    }
    lapic_enable_this_cpu();

    kprintf("[APIC] %s mode, BSP APIC ID %u, version 0x%x\n", x2apic_mode ? "x2APIC" : "xAPIC", lapic_id(),
            lapic_read(LAPIC_VERSION) & 0xFF);
}

void lapic_init_ap(void) {
    lapic_enable_this_cpu();
}

void lapic_eoi(void) {
    if (x2apic_mode) {
        wrmsr(MSR_X2APIC_FIRST + (LAPIC_EOI >> 4), 0);
    } else if (lapic_mmio) {
        lapic_mmio[LAPIC_EOI / 4] = 0;
    }
}

uint32_t lapic_id(void) {
    uint32_t id = lapic_read(LAPIC_ID);
    return x2apic_mode ? id : (id >> 24) & 0xFF;
}

/* Write the ICR. x2APIC: one 64-bit MSR write, destination in bits 63:32, no
 * delivery-status bit. xAPIC: high half, then low half (which sends), then wait
 * until the APIC accepted it. */
static void lapic_write_icr(uint32_t dest, uint32_t low) {
    if (x2apic_mode) {
        /* x2APIC MSR writes are not serializing: make earlier stores visible to
         * the target before it can take the interrupt */
        __asm__ volatile("mfence; lfence" : : : "memory");
        wrmsr(MSR_X2APIC_FIRST + (LAPIC_ICR_LOW >> 4), ((uint64_t)dest << 32) | low);
        return;
    }
    uint64_t flags = irq_save();
    lapic_mmio[LAPIC_ICR_HIGH / 4] = dest << 24;
    lapic_mmio[LAPIC_ICR_LOW / 4] = low;
    while (lapic_mmio[LAPIC_ICR_LOW / 4] & ICR_SEND_PENDING) {
        __asm__ volatile ("pause");
    }
    irq_restore(flags);
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    lapic_write_icr(apic_id, vector | ICR_FIXED | ICR_LEVEL_ASSERT);
}

void lapic_send_ipi_all_excluding_self(uint32_t vector) {
    lapic_write_icr(0, vector | ICR_FIXED | ICR_DEST_ALL_EX | ICR_LEVEL_ASSERT);
}

void lapic_timer_init(uint32_t vector, uint32_t freq_hz) {
    /* Set divide value to 16 */
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);

    /* Calibrate: use PIT to measure LAPIC timer speed.
     * Set initial count to max, wait ~10ms, read remaining. */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* Busy-wait ~10ms using PIT ticks (PIT runs at 100 Hz = 10ms/tick) */
    uint64_t start = pit_get_ticks();
    while (pit_get_ticks() - start < 1) {
        __asm__ volatile ("pause");
    }

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

    /* Calculate initial count for desired frequency.
     * elapsed ticks in ~10ms → elapsed * 100 = ticks/second.
     * For freq_hz: initial_count = ticks_per_second / freq_hz */
    uint32_t ticks_per_sec = elapsed * 100;
    uint32_t initial_count = ticks_per_sec / freq_hz;

    if (initial_count == 0) initial_count = 1;

    /* Configure timer: periodic mode, desired vector */
    lapic_write(LAPIC_TIMER_LVT, vector | (1 << 17)); /* bit 17 = periodic */
    lapic_write(LAPIC_TIMER_ICR, initial_count);

    kprintf("[LAPIC] Timer: %u ticks/sec, period=%u for %u Hz\n",
            ticks_per_sec, initial_count, freq_hz);
}
