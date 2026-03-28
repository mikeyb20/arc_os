#ifndef ARCHOS_DRIVERS_ACPI_H
#define ARCHOS_DRIVERS_ACPI_H

#include <stdint.h>

/* RSDP — Root System Description Pointer (ACPI 1.0, 20 bytes) */
typedef struct __attribute__((packed)) {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2+ = ACPI 2.0+ */
    uint32_t rsdt_address;   /* 32-bit physical address of RSDT */
} AcpiRsdp;

/* RSDP 2.0 extension (total 36 bytes) */
typedef struct __attribute__((packed)) {
    AcpiRsdp v1;
    uint32_t length;
    uint64_t xsdt_address;   /* 64-bit physical address of XSDT */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} AcpiRsdp2;

/* SDT header — common header for all ACPI tables */
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} AcpiSdtHeader;

/* RSDT — Root System Description Table (32-bit pointers) */
typedef struct __attribute__((packed)) {
    AcpiSdtHeader header;
    uint32_t      entries[];  /* Array of 32-bit physical addresses */
} AcpiRsdt;

/* XSDT — Extended System Description Table (64-bit pointers) */
typedef struct __attribute__((packed)) {
    AcpiSdtHeader header;
    uint64_t      entries[];  /* Array of 64-bit physical addresses */
} AcpiXsdt;

/* MADT — Multiple APIC Description Table */
typedef struct __attribute__((packed)) {
    AcpiSdtHeader header;
    uint32_t      local_apic_address;
    uint32_t      flags;      /* Bit 0: dual 8259 PICs installed */
    /* Variable-length entries follow */
} AcpiMadt;

/* MADT entry header */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} AcpiMadtEntry;

/* MADT entry types */
#define MADT_ENTRY_LOCAL_APIC    0
#define MADT_ENTRY_IO_APIC       1
#define MADT_ENTRY_ISO            2  /* Interrupt Source Override */
#define MADT_ENTRY_NMI            4  /* Non-Maskable Interrupt */
#define MADT_ENTRY_LOCAL_APIC_64  5  /* 64-bit Local APIC Address Override */

/* MADT Local APIC entry (type 0) */
typedef struct __attribute__((packed)) {
    AcpiMadtEntry header;
    uint8_t       processor_id;
    uint8_t       apic_id;
    uint32_t      flags;      /* Bit 0: processor enabled */
} AcpiMadtLocalApic;

/* MADT I/O APIC entry (type 1) */
typedef struct __attribute__((packed)) {
    AcpiMadtEntry header;
    uint8_t       io_apic_id;
    uint8_t       reserved;
    uint32_t      io_apic_address;
    uint32_t      gsi_base;   /* Global System Interrupt base */
} AcpiMadtIoApic;

/* MADT Interrupt Source Override entry (type 2) */
typedef struct __attribute__((packed)) {
    AcpiMadtEntry header;
    uint8_t       bus;        /* Always 0 (ISA) */
    uint8_t       source;     /* ISA IRQ number */
    uint32_t      gsi;        /* Global System Interrupt */
    uint16_t      flags;      /* Polarity + trigger mode */
} AcpiMadtIso;

/* Parsed ACPI info — populated by acpi_init() */
#define ACPI_MAX_CPUS     16
#define ACPI_MAX_ISOS     16

typedef struct {
    /* CPU info from MADT */
    uint8_t  cpu_apic_ids[ACPI_MAX_CPUS];
    uint32_t cpu_count;

    /* I/O APIC */
    uint32_t io_apic_address;
    uint8_t  io_apic_id;

    /* Local APIC address (may be overridden by 64-bit entry) */
    uint64_t local_apic_address;

    /* Interrupt source overrides */
    AcpiMadtIso isos[ACPI_MAX_ISOS];
    uint32_t    iso_count;

    /* Dual 8259 legacy PICs present */
    int has_legacy_pics;
} AcpiInfo;

/* Parse ACPI tables starting from the RSDP physical address.
 * Populates the global AcpiInfo struct.
 * Returns 0 on success, -1 on failure. */
int acpi_init(uint64_t rsdp_phys);

/* Get parsed ACPI info.  Returns NULL if acpi_init has not been called. */
const AcpiInfo *acpi_get_info(void);

#endif /* ARCHOS_DRIVERS_ACPI_H */
