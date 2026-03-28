/* arc_os — Local APIC driver
 *
 * The Local APIC is memory-mapped at the address from MADT.
 * We use the HHDM to access it. */

#include "arch/x86_64/lapic.h"
#include "arch/x86_64/pit.h"
#include "lib/kprintf.h"
#include <stddef.h>

static volatile uint32_t *lapic_base;

uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

void lapic_init(uint64_t base_virt) {
    lapic_base = (volatile uint32_t *)base_virt;

    /* Enable LAPIC via Spurious Interrupt Vector Register */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);

    /* Clear Error Status Register (write twice as per spec) */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* Set Task Priority Register to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* Send EOI to clear any pending interrupts */
    lapic_eoi();

    kprintf("[LAPIC] Initialized (ID=%u, version=0x%x)\n",
            lapic_id(), lapic_read(LAPIC_VERSION) & 0xFF);
}

void lapic_eoi(void) {
    if (lapic_base)
        lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id(void) {
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* Wait for IPI delivery (ICR delivery status bit) */
static void lapic_ipi_wait(void) {
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12)) {
        __asm__ volatile ("pause");
    }
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector | ICR_FIXED | ICR_LEVEL_ASSERT);
    lapic_ipi_wait();
}

void lapic_send_ipi_all(uint32_t vector) {
    lapic_write(LAPIC_ICR_LOW, vector | ICR_FIXED | ICR_DEST_ALL | ICR_LEVEL_ASSERT);
    lapic_ipi_wait();
}

void lapic_send_ipi_all_excluding_self(uint32_t vector) {
    lapic_write(LAPIC_ICR_LOW, vector | ICR_FIXED | ICR_DEST_ALL_EX | ICR_LEVEL_ASSERT);
    lapic_ipi_wait();
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
