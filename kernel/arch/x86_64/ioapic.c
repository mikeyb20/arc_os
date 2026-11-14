/* arc_os — I/O APIC driver
 *
 * Programs I/O APIC redirection table entries to route IRQs to
 * LAPIC vectors. Uses MADT Interrupt Source Overrides for remapping. */

#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/isr.h"
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

void ioapic_route_isa(const AcpiInfo *acpi, uint8_t dest_apic) {
    /* Route ISA IRQs 0-15 to vectors 32+irq on dest_apic, applying the MADT
     * Interrupt Source Overrides (moved here from kmain, M1-03) */
    for (uint8_t irq = 0; irq < 16; irq++) {
        uint8_t gsi = irq;
        int active_low = 0, level = 0;

        /* Check for Interrupt Source Overrides */
        for (uint32_t j = 0; j < acpi->iso_count; j++) {
            if (acpi->isos[j].source == irq) {
                gsi = (uint8_t)acpi->isos[j].gsi;
                uint16_t flags = acpi->isos[j].flags;
                active_low = ((flags & 0x3) == 3);
                level = (((flags >> 2) & 0x3) == 3);
                break;
            }
        }

        /* A GSI claimed by another IRQ's override (e.g. PIT IRQ0 ->
         * GSI2) must not be re-routed by the identity mapping of the
         * IRQ with the same number, or the timer lands on vector 34. */
        int claimed = 0;
        if (gsi == irq) {
            for (uint32_t j = 0; j < acpi->iso_count; j++) {
                if (acpi->isos[j].gsi == irq && acpi->isos[j].source != irq) {
                    claimed = 1;
                    break;
                }
            }
        }
        if (claimed) continue;

        ioapic_route_irq(gsi, (uint8_t)(IRQ_BASE + irq), dest_apic, active_low, level);
        /* IRQ0 is the PIT, used only for calibration since M1-06: kept masked */
        if (irq == 0)
            ioapic_mask(gsi);
        else
            ioapic_unmask(gsi);
    }
}
