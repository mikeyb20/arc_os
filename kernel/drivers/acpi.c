#include "drivers/acpi.h"
#include "mm/vmm.h"
#include "lib/kprintf.h"
#include "lib/mem.h"

static AcpiInfo acpi_info;
static int acpi_initialized;

/* Convert a physical address to a virtual address via HHDM. */
static void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + vmm_get_hhdm_offset());
}

/* Validate an ACPI table checksum (sum of all bytes must be 0). */
static int acpi_checksum(const void *data, uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

/* Parse MADT (Multiple APIC Description Table). */
static void parse_madt(const AcpiMadt *madt) {
    acpi_info.local_apic_address = madt->local_apic_address;
    acpi_info.has_legacy_pics = (madt->flags & 1);

    kprintf("[ACPI] MADT: Local APIC at 0x%lx, legacy PICs=%s\n",
            acpi_info.local_apic_address,
            acpi_info.has_legacy_pics ? "yes" : "no");

    /* Walk variable-length MADT entries */
    const uint8_t *ptr = (const uint8_t *)madt + sizeof(AcpiMadt);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (ptr + 2 <= end) {
        const AcpiMadtEntry *entry = (const AcpiMadtEntry *)ptr;
        if (entry->length < 2 || ptr + entry->length > end) break;

        switch (entry->type) {
        case MADT_ENTRY_LOCAL_APIC: {
            const AcpiMadtLocalApic *lapic = (const AcpiMadtLocalApic *)entry;
            if ((lapic->flags & 1) && acpi_info.cpu_count < ACPI_MAX_CPUS) {
                acpi_info.cpu_apic_ids[acpi_info.cpu_count] = lapic->apic_id;
                acpi_info.cpu_count++;
                kprintf("[ACPI]   CPU: processor_id=%u apic_id=%u\n",
                        lapic->processor_id, lapic->apic_id);
            }
            break;
        }
        case MADT_ENTRY_IO_APIC: {
            const AcpiMadtIoApic *ioapic = (const AcpiMadtIoApic *)entry;
            acpi_info.io_apic_address = ioapic->io_apic_address;
            acpi_info.io_apic_id = ioapic->io_apic_id;
            kprintf("[ACPI]   I/O APIC: id=%u addr=0x%x gsi_base=%u\n",
                    ioapic->io_apic_id, ioapic->io_apic_address, ioapic->gsi_base);
            break;
        }
        case MADT_ENTRY_ISO: {
            const AcpiMadtIso *iso = (const AcpiMadtIso *)entry;
            if (acpi_info.iso_count < ACPI_MAX_ISOS) {
                acpi_info.isos[acpi_info.iso_count] = *iso;
                acpi_info.iso_count++;
                kprintf("[ACPI]   ISO: bus=%u source=%u gsi=%u flags=0x%x\n",
                        iso->bus, iso->source, iso->gsi, iso->flags);
            }
            break;
        }
        case MADT_ENTRY_LOCAL_APIC_64: {
            /* 64-bit Local APIC Address Override */
            if (entry->length >= 12) {
                uint64_t addr;
                memcpy(&addr, ptr + 4, 8);
                acpi_info.local_apic_address = addr;
                kprintf("[ACPI]   Local APIC 64-bit override: 0x%lx\n", addr);
            }
            break;
        }
        default:
            break;
        }

        ptr += entry->length;
    }
}

/* Process an SDT entry (check signature, dispatch to parser). */
static void process_sdt(uint64_t phys) {
    const AcpiSdtHeader *header = (const AcpiSdtHeader *)phys_to_virt(phys);

    if (!acpi_checksum(header, header->length)) {
        kprintf("[ACPI] Bad checksum for table '%.4s'\n", header->signature);
        return;
    }

    kprintf("[ACPI] Found table '%.4s' (length=%u)\n",
            header->signature, header->length);

    if (memcmp(header->signature, "APIC", 4) == 0) {
        parse_madt((const AcpiMadt *)header);
    }
    /* Future: FACP (FADT), HPET, MCFG, etc. */
}

int acpi_init(uint64_t rsdp_phys) {
    if (rsdp_phys == 0) {
        kprintf("[ACPI] No RSDP address provided\n");
        return -1;
    }

    memset(&acpi_info, 0, sizeof(acpi_info));

    const AcpiRsdp *rsdp = (const AcpiRsdp *)phys_to_virt(rsdp_phys);

    /* Validate RSDP signature */
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        kprintf("[ACPI] Invalid RSDP signature\n");
        return -1;
    }

    /* Validate RSDP v1 checksum (first 20 bytes) */
    if (!acpi_checksum(rsdp, 20)) {
        kprintf("[ACPI] Bad RSDP checksum\n");
        return -1;
    }

    kprintf("[ACPI] RSDP revision=%u OEM='%.6s'\n", rsdp->revision, rsdp->oem_id);

    if (rsdp->revision >= 2) {
        /* ACPI 2.0+ — use XSDT (64-bit pointers) */
        const AcpiRsdp2 *rsdp2 = (const AcpiRsdp2 *)rsdp;
        if (!acpi_checksum(rsdp2, rsdp2->length)) {
            kprintf("[ACPI] Bad RSDP 2.0 extended checksum\n");
            return -1;
        }

        if (rsdp2->xsdt_address != 0) {
            const AcpiXsdt *xsdt = (const AcpiXsdt *)phys_to_virt(rsdp2->xsdt_address);
            if (memcmp(xsdt->header.signature, "XSDT", 4) != 0) {
                kprintf("[ACPI] Invalid XSDT signature\n");
                return -1;
            }
            if (!acpi_checksum(xsdt, xsdt->header.length)) {
                kprintf("[ACPI] Bad XSDT checksum\n");
                return -1;
            }

            uint32_t entry_count = (xsdt->header.length - sizeof(AcpiSdtHeader)) / 8;
            kprintf("[ACPI] XSDT has %u entries\n", entry_count);

            for (uint32_t i = 0; i < entry_count; i++) {
                process_sdt(xsdt->entries[i]);
            }

            acpi_initialized = 1;
            kprintf("[ACPI] Initialized (%u CPUs found)\n", acpi_info.cpu_count);
            return 0;
        }
    }

    /* ACPI 1.0 or XSDT not available — use RSDT (32-bit pointers) */
    if (rsdp->rsdt_address == 0) {
        kprintf("[ACPI] No RSDT address\n");
        return -1;
    }

    const AcpiRsdt *rsdt = (const AcpiRsdt *)phys_to_virt(rsdp->rsdt_address);
    if (memcmp(rsdt->header.signature, "RSDT", 4) != 0) {
        kprintf("[ACPI] Invalid RSDT signature\n");
        return -1;
    }
    if (!acpi_checksum(rsdt, rsdt->header.length)) {
        kprintf("[ACPI] Bad RSDT checksum\n");
        return -1;
    }

    uint32_t entry_count = (rsdt->header.length - sizeof(AcpiSdtHeader)) / 4;
    kprintf("[ACPI] RSDT has %u entries\n", entry_count);

    for (uint32_t i = 0; i < entry_count; i++) {
        process_sdt((uint64_t)rsdt->entries[i]);
    }

    acpi_initialized = 1;
    kprintf("[ACPI] Initialized (%u CPUs found)\n", acpi_info.cpu_count);
    return 0;
}

const AcpiInfo *acpi_get_info(void) {
    return acpi_initialized ? &acpi_info : NULL;
}
