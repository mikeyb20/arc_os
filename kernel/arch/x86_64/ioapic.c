/* arc_os — I/O APIC driver
 *
 * Programs I/O APIC redirection table entries to route IRQs to
 * LAPIC vectors. Uses MADT Interrupt Source Overrides for remapping. */

#include "arch/x86_64/ioapic.h"
#include "drivers/acpi.h"
#include "lib/kprintf.h"

static volatile uint32_t *ioapic_base;
static uint32_t max_entries;

uint32_t ioapic_read(uint32_t reg) {
    ioapic_base[IOAPIC_REGSEL / 4] = reg;
    return ioapic_base[IOAPIC_IOWIN / 4];
}

void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic_base[IOAPIC_REGSEL / 4] = reg;
    ioapic_base[IOAPIC_IOWIN / 4] = val;
}

/* Read a 64-bit redirection entry */
static uint64_t ioapic_read_redir(uint8_t irq) {
    uint32_t reg = IOAPIC_REG_REDIR + 2 * irq;
    uint64_t lo = ioapic_read(reg);
    uint64_t hi = ioapic_read(reg + 1);
    return lo | (hi << 32);
}

/* Write a 64-bit redirection entry */
static void ioapic_write_redir(uint8_t irq, uint64_t entry) {
    uint32_t reg = IOAPIC_REG_REDIR + 2 * irq;
    ioapic_write(reg, (uint32_t)(entry & 0xFFFFFFFF));
    ioapic_write(reg + 1, (uint32_t)(entry >> 32));
}

void ioapic_init(uint64_t base_virt) {
    ioapic_base = (volatile uint32_t *)base_virt;

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    max_entries = ((ver >> 16) & 0xFF) + 1;

    /* Mask all entries initially */
    for (uint32_t i = 0; i < max_entries; i++) {
        uint64_t entry = ioapic_read_redir((uint8_t)i);
        entry |= IOAPIC_MASKED;
        ioapic_write_redir((uint8_t)i, entry);
    }

    kprintf("[IOAPIC] Initialized (max entries=%u)\n", max_entries);
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic,
                       int active_low, int level_triggered) {
    if (irq >= max_entries) return;

    uint64_t entry = (uint64_t)vector;
    if (active_low)       entry |= IOAPIC_ACTIVE_LOW;
    if (level_triggered)  entry |= IOAPIC_LEVEL;
    entry |= ((uint64_t)dest_apic << 56);  /* Destination APIC ID */

    ioapic_write_redir(irq, entry);
}

void ioapic_mask(uint8_t irq) {
    if (irq >= max_entries) return;
    uint64_t entry = ioapic_read_redir(irq);
    entry |= IOAPIC_MASKED;
    ioapic_write_redir(irq, entry);
}

void ioapic_unmask(uint8_t irq) {
    if (irq >= max_entries) return;
    uint64_t entry = ioapic_read_redir(irq);
    entry &= ~IOAPIC_MASKED;
    ioapic_write_redir(irq, entry);
}
