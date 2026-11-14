#include "drivers/acpi.h"
#include "mm/vmm.h"
#include "lib/kprintf.h"
#include "lib/mem.h"

#define ACPI_DMAR_FLAGS_OFFSET 37   /* after the header and the host address width */
#define ACPI_HPET_GAS_OFFSET     40   /* HPET table: base address (Generic Address Structure) */
#define ACPI_HPET_MIN_LEN        56
#define ACPI_FADT_CENTURY_OFFSET 108  /* FADT: CMOS index of the RTC century register */

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

/* Record an enabled CPU once (firmware may list a CPU as both type 0 and 9).
 * Runs once per MADT entry at boot: the linear duplicate check is boot-only. */
static void madt_add_cpu(uint32_t apic_id, uint32_t flags) {
    if (!(flags & MADT_LAPIC_ENABLED)) return;   /* online-capable: hot-add, not supported */
    for (uint32_t i = 0; i < acpi_info.cpu_count; i++) {
        if (acpi_info.cpu_apic_ids[i] == apic_id) return;
    }
    if (acpi_info.cpu_count >= ACPI_MAX_CPUS) {
        kprintf("[ACPI]   CPU apic_id=%u ignored: more than %u CPUs\n", apic_id, ACPI_MAX_CPUS);
        return;
    }
    acpi_info.cpu_apic_ids[acpi_info.cpu_count++] = apic_id;
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
            if (entry->length < sizeof(*lapic) || lapic->apic_id == 0xFF) break;
            madt_add_cpu(lapic->apic_id, lapic->flags);
            break;
        }
        case MADT_ENTRY_LOCAL_X2APIC: {
            const AcpiMadtLocalX2Apic *x2 = (const AcpiMadtLocalX2Apic *)entry;
            if (entry->length < sizeof(*x2) || x2->x2apic_id == 0xFFFFFFFF) break;
            madt_add_cpu(x2->x2apic_id, x2->flags);
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

/* Remember a listed table for the boot report. */
static void record_table(const AcpiSdtHeader *header, int checksum_ok) {
    acpi_info.tables_listed++;
    if (acpi_info.table_count >= ACPI_MAX_TABLES) return;
    AcpiTableEntry *t = &acpi_info.tables[acpi_info.table_count++];
    memcpy(t->signature, header->signature, 4);
    t->signature[4] = '\0';
    t->length = header->length;
    t->checksum_ok = checksum_ok;
}

/* Process an SDT entry (check signature, dispatch to parser). */
static void process_sdt(uint64_t phys) {
    const AcpiSdtHeader *header = (const AcpiSdtHeader *)phys_to_virt(phys);

    if (!acpi_checksum(header, header->length)) {
        kprintf("[ACPI] Bad checksum for table '%.4s'\n", header->signature);
        record_table(header, 0);
        return;
    }
    record_table(header, 1);

    kprintf("[ACPI] Found table '%.4s' (length=%u)\n",
            header->signature, header->length);

    if (memcmp(header->signature, "APIC", 4) == 0) {
        parse_madt((const AcpiMadt *)header);
    } else if (memcmp(header->signature, "DMAR", 4) == 0 && header->length > ACPI_DMAR_FLAGS_OFFSET) {
        acpi_info.dmar_present = 1;
        acpi_info.dmar_flags = ((const uint8_t *)header)[ACPI_DMAR_FLAGS_OFFSET];
        kprintf("[ACPI] DMAR: interrupt remapping %s, x2APIC opt-out %s\n",
                (acpi_info.dmar_flags & ACPI_DMAR_INTR_REMAP) ? "supported" : "no",
                (acpi_info.dmar_flags & ACPI_DMAR_X2APIC_OPT_OUT) ? "SET" : "no");
    } else if (memcmp(header->signature, "HPET", 4) == 0 && header->length >= ACPI_HPET_MIN_LEN) {
        /* Generic Address Structure at 40: address space ID 0 = memory, address at 44 */
        const uint8_t *t = (const uint8_t *)header;
        if (t[ACPI_HPET_GAS_OFFSET] == 0) memcpy(&acpi_info.hpet_address, t + ACPI_HPET_GAS_OFFSET + 4, 8);
        kprintf("[ACPI] HPET at 0x%lx\n", acpi_info.hpet_address);
    } else if (memcmp(header->signature, "FACP", 4) == 0 && header->length > ACPI_FADT_CENTURY_OFFSET) {
        acpi_info.century_reg = ((const uint8_t *)header)[ACPI_FADT_CENTURY_OFFSET];
    }
    /* Future: MCFG, etc. */
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
    acpi_info.rsdp_revision = rsdp->revision;
    memcpy(acpi_info.oem_id, rsdp->oem_id, 6);
    acpi_info.oem_id[6] = '\0';

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
