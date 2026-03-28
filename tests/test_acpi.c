/* arc_os — Host-side tests for ACPI table parsing */

#define _GNU_SOURCE
#include "test_framework.h"
#include <stdint.h>
#include <sys/mman.h>

/* Guard kernel headers */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_STRING_H
#define ARCHOS_MM_VMM_H

static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* VMM stub — HHDM offset is 0 for tests (phys == virt) */
static uint64_t vmm_get_hhdm_offset(void) { return 0; }

/* Include ACPI types inline (avoid needing full headers) */
#include "../kernel/drivers/acpi.c"

/* Helper: compute checksum byte so sum of region == 0 */
static uint8_t calc_checksum(const void *data, uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return (uint8_t)(0 - sum);
}

/* --- Test: RSDP validation --- */

TEST(rsdp_bad_signature) {
    AcpiRsdp rsdp;
    memset(&rsdp, 0, sizeof(rsdp));
    memcpy(rsdp.signature, "BAD SIG!", 8);
    ASSERT_EQ(acpi_init((uint64_t)&rsdp), -1);
    return 0;
}

TEST(rsdp_bad_checksum) {
    AcpiRsdp rsdp;
    memset(&rsdp, 0, sizeof(rsdp));
    memcpy(rsdp.signature, "RSD PTR ", 8);
    rsdp.checksum = 0xFF; /* Bad checksum */
    ASSERT_EQ(acpi_init((uint64_t)&rsdp), -1);
    return 0;
}

TEST(rsdp_null_address) {
    ASSERT_EQ(acpi_init(0), -1);
    return 0;
}

/* --- Test: RSDT parsing with MADT --- */

/* Allocate a 4KB buffer in the low 2GB so 32-bit RSDT pointers work.
 * Falls back to regular malloc if MAP_32BIT is unavailable. */
#define TEST_BUF_SIZE 4096
static uint8_t *test_acpi_buf;

static void ensure_test_buf(void) {
    if (test_acpi_buf) return;
#ifdef MAP_32BIT
    test_acpi_buf = mmap(NULL, TEST_BUF_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (test_acpi_buf == MAP_FAILED) test_acpi_buf = NULL;
#endif
    if (!test_acpi_buf) test_acpi_buf = malloc(TEST_BUF_SIZE);
}

static void build_test_madt(uint8_t *buf, uint32_t *out_len,
                             uint32_t lapic_addr, int cpu_count) {
    AcpiMadt *madt = (AcpiMadt *)buf;
    memcpy(madt->header.signature, "APIC", 4);
    madt->header.revision = 1;
    madt->local_apic_address = lapic_addr;
    madt->flags = 1; /* Legacy PICs */

    uint8_t *ptr = buf + sizeof(AcpiMadt);

    /* Add Local APIC entries */
    for (int i = 0; i < cpu_count; i++) {
        AcpiMadtLocalApic *lapic = (AcpiMadtLocalApic *)ptr;
        lapic->header.type = MADT_ENTRY_LOCAL_APIC;
        lapic->header.length = sizeof(AcpiMadtLocalApic);
        lapic->processor_id = (uint8_t)i;
        lapic->apic_id = (uint8_t)i;
        lapic->flags = 1; /* Enabled */
        ptr += sizeof(AcpiMadtLocalApic);
    }

    /* Add an I/O APIC entry */
    AcpiMadtIoApic *ioapic = (AcpiMadtIoApic *)ptr;
    ioapic->header.type = MADT_ENTRY_IO_APIC;
    ioapic->header.length = sizeof(AcpiMadtIoApic);
    ioapic->io_apic_id = 0;
    ioapic->io_apic_address = 0xFEC00000;
    ioapic->gsi_base = 0;
    ptr += sizeof(AcpiMadtIoApic);

    uint32_t total = (uint32_t)(ptr - buf);
    madt->header.length = total;
    madt->header.checksum = 0;
    madt->header.checksum = calc_checksum(madt, total);
    *out_len = total;
}

TEST(rsdt_single_cpu) {
    ensure_test_buf();
    memset(test_acpi_buf, 0, TEST_BUF_SIZE);

    /* Layout: [RSDP at 0] [RSDT at 256] [MADT at 512] */
    AcpiRsdp *rsdp = (AcpiRsdp *)&test_acpi_buf[0];
    AcpiRsdt *rsdt = (AcpiRsdt *)&test_acpi_buf[256];
    uint8_t  *madt_buf = &test_acpi_buf[512];

    /* Build MADT */
    uint32_t madt_len;
    build_test_madt(madt_buf, &madt_len, 0xFEE00000, 1);

    /* Build RSDT with one entry pointing to MADT */
    memcpy(rsdt->header.signature, "RSDT", 4);
    rsdt->header.revision = 1;
    rsdt->header.length = sizeof(AcpiSdtHeader) + 4; /* One 32-bit entry */
    rsdt->entries[0] = (uint32_t)(uintptr_t)madt_buf;
    rsdt->header.checksum = 0;
    rsdt->header.checksum = calc_checksum(rsdt, rsdt->header.length);

    /* Build RSDP */
    memcpy(rsdp->signature, "RSD PTR ", 8);
    rsdp->revision = 0; /* ACPI 1.0 */
    rsdp->rsdt_address = (uint32_t)(uintptr_t)rsdt;
    rsdp->checksum = 0;
    rsdp->checksum = calc_checksum(rsdp, 20);

    ASSERT_EQ(acpi_init((uint64_t)(uintptr_t)rsdp), 0);

    const AcpiInfo *info = acpi_get_info();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->cpu_count, 1);
    ASSERT_EQ(info->cpu_apic_ids[0], 0);
    ASSERT_EQ(info->local_apic_address, 0xFEE00000);
    ASSERT_EQ(info->io_apic_address, 0xFEC00000);
    ASSERT_EQ(info->has_legacy_pics, 1);
    return 0;
}

TEST(rsdt_four_cpus) {
    ensure_test_buf();
    memset(test_acpi_buf, 0, TEST_BUF_SIZE);

    AcpiRsdp *rsdp = (AcpiRsdp *)&test_acpi_buf[0];
    AcpiRsdt *rsdt = (AcpiRsdt *)&test_acpi_buf[256];
    uint8_t  *madt_buf = &test_acpi_buf[512];

    uint32_t madt_len;
    build_test_madt(madt_buf, &madt_len, 0xFEE00000, 4);

    memcpy(rsdt->header.signature, "RSDT", 4);
    rsdt->header.revision = 1;
    rsdt->header.length = sizeof(AcpiSdtHeader) + 4;
    rsdt->entries[0] = (uint32_t)(uintptr_t)madt_buf;
    rsdt->header.checksum = 0;
    rsdt->header.checksum = calc_checksum(rsdt, rsdt->header.length);

    memcpy(rsdp->signature, "RSD PTR ", 8);
    rsdp->revision = 0;
    rsdp->rsdt_address = (uint32_t)(uintptr_t)rsdt;
    rsdp->checksum = 0;
    rsdp->checksum = calc_checksum(rsdp, 20);

    ASSERT_EQ(acpi_init((uint64_t)(uintptr_t)rsdp), 0);

    const AcpiInfo *info = acpi_get_info();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->cpu_count, 4);
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT_EQ(info->cpu_apic_ids[i], i);
    }
    return 0;
}

TEST(xsdt_parsing) {
    ensure_test_buf();
    memset(test_acpi_buf, 0, TEST_BUF_SIZE);

    /* Layout: [RSDP2 at 0] [XSDT at 256] [MADT at 512] */
    AcpiRsdp2 *rsdp2 = (AcpiRsdp2 *)&test_acpi_buf[0];
    AcpiXsdt  *xsdt  = (AcpiXsdt *)&test_acpi_buf[256];
    uint8_t   *madt_buf = &test_acpi_buf[512];

    uint32_t madt_len;
    build_test_madt(madt_buf, &madt_len, 0xFEE00000, 2);

    /* Build XSDT with one 64-bit entry */
    memcpy(xsdt->header.signature, "XSDT", 4);
    xsdt->header.revision = 1;
    xsdt->header.length = sizeof(AcpiSdtHeader) + 8;
    xsdt->entries[0] = (uint64_t)(uintptr_t)madt_buf;
    xsdt->header.checksum = 0;
    xsdt->header.checksum = calc_checksum(xsdt, xsdt->header.length);

    /* Build RSDP 2.0 */
    memcpy(rsdp2->v1.signature, "RSD PTR ", 8);
    rsdp2->v1.revision = 2; /* ACPI 2.0+ */
    rsdp2->v1.rsdt_address = 0;
    rsdp2->length = 36;
    rsdp2->xsdt_address = (uint64_t)(uintptr_t)xsdt;
    /* Set v1 checksum */
    rsdp2->v1.checksum = 0;
    rsdp2->v1.checksum = calc_checksum(&rsdp2->v1, 20);
    /* Set extended checksum */
    rsdp2->ext_checksum = 0;
    rsdp2->ext_checksum = calc_checksum(rsdp2, 36);

    ASSERT_EQ(acpi_init((uint64_t)(uintptr_t)rsdp2), 0);

    const AcpiInfo *info = acpi_get_info();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->cpu_count, 2);
    return 0;
}

TEST(acpi_get_info_before_init) {
    acpi_initialized = 0;
    ASSERT_TRUE(acpi_get_info() == NULL);
    return 0;
}

TEST(rsdt_bad_signature) {
    ensure_test_buf();
    memset(test_acpi_buf, 0, TEST_BUF_SIZE);

    AcpiRsdp *rsdp = (AcpiRsdp *)&test_acpi_buf[0];
    AcpiRsdt *rsdt = (AcpiRsdt *)&test_acpi_buf[256];

    /* RSDT with wrong signature */
    memcpy(rsdt->header.signature, "BAAD", 4);
    rsdt->header.length = sizeof(AcpiSdtHeader);
    rsdt->header.checksum = 0;
    rsdt->header.checksum = calc_checksum(rsdt, rsdt->header.length);

    memcpy(rsdp->signature, "RSD PTR ", 8);
    rsdp->revision = 0;
    rsdp->rsdt_address = (uint32_t)(uintptr_t)rsdt;
    rsdp->checksum = 0;
    rsdp->checksum = calc_checksum(rsdp, 20);

    ASSERT_EQ(acpi_init((uint64_t)(uintptr_t)rsdp), -1);
    return 0;
}

TEST(madt_iso_entries) {
    ensure_test_buf();
    memset(test_acpi_buf, 0, TEST_BUF_SIZE);

    AcpiRsdp *rsdp = (AcpiRsdp *)&test_acpi_buf[0];
    AcpiRsdt *rsdt = (AcpiRsdt *)&test_acpi_buf[256];
    uint8_t  *madt_start = &test_acpi_buf[512];

    /* Build MADT with 1 CPU + 1 ISO */
    AcpiMadt *madt = (AcpiMadt *)madt_start;
    memcpy(madt->header.signature, "APIC", 4);
    madt->header.revision = 1;
    madt->local_apic_address = 0xFEE00000;
    madt->flags = 1;

    uint8_t *ptr = madt_start + sizeof(AcpiMadt);

    /* Local APIC */
    AcpiMadtLocalApic *lapic = (AcpiMadtLocalApic *)ptr;
    lapic->header.type = MADT_ENTRY_LOCAL_APIC;
    lapic->header.length = sizeof(AcpiMadtLocalApic);
    lapic->processor_id = 0;
    lapic->apic_id = 0;
    lapic->flags = 1;
    ptr += sizeof(AcpiMadtLocalApic);

    /* ISO: IRQ 0 → GSI 2 */
    AcpiMadtIso *iso = (AcpiMadtIso *)ptr;
    iso->header.type = MADT_ENTRY_ISO;
    iso->header.length = sizeof(AcpiMadtIso);
    iso->bus = 0;
    iso->source = 0;
    iso->gsi = 2;
    iso->flags = 0;
    ptr += sizeof(AcpiMadtIso);

    /* I/O APIC */
    AcpiMadtIoApic *ioapic = (AcpiMadtIoApic *)ptr;
    ioapic->header.type = MADT_ENTRY_IO_APIC;
    ioapic->header.length = sizeof(AcpiMadtIoApic);
    ioapic->io_apic_id = 0;
    ioapic->io_apic_address = 0xFEC00000;
    ioapic->gsi_base = 0;
    ptr += sizeof(AcpiMadtIoApic);

    uint32_t total = (uint32_t)(ptr - madt_start);
    madt->header.length = total;
    madt->header.checksum = 0;
    madt->header.checksum = calc_checksum(madt, total);

    /* Build RSDT */
    memcpy(rsdt->header.signature, "RSDT", 4);
    rsdt->header.length = sizeof(AcpiSdtHeader) + 4;
    rsdt->entries[0] = (uint32_t)(uintptr_t)madt_start;
    rsdt->header.checksum = 0;
    rsdt->header.checksum = calc_checksum(rsdt, rsdt->header.length);

    /* Build RSDP */
    memcpy(rsdp->signature, "RSD PTR ", 8);
    rsdp->revision = 0;
    rsdp->rsdt_address = (uint32_t)(uintptr_t)rsdt;
    rsdp->checksum = 0;
    rsdp->checksum = calc_checksum(rsdp, 20);

    ASSERT_EQ(acpi_init((uint64_t)(uintptr_t)rsdp), 0);

    const AcpiInfo *info = acpi_get_info();
    ASSERT_TRUE(info != NULL);
    ASSERT_EQ(info->cpu_count, 1);
    ASSERT_EQ(info->iso_count, 1);
    uint8_t iso_source = info->isos[0].source;
    uint32_t iso_gsi = info->isos[0].gsi;
    ASSERT_EQ(iso_source, 0);
    ASSERT_EQ(iso_gsi, 2);
    return 0;
}

/* --- Suite --- */

TestCase acpi_tests[] = {
    TEST_ENTRY(rsdp_bad_signature),
    TEST_ENTRY(rsdp_bad_checksum),
    TEST_ENTRY(rsdp_null_address),
    TEST_ENTRY(rsdt_single_cpu),
    TEST_ENTRY(rsdt_four_cpus),
    TEST_ENTRY(xsdt_parsing),
    TEST_ENTRY(acpi_get_info_before_init),
    TEST_ENTRY(rsdt_bad_signature),
    TEST_ENTRY(madt_iso_entries),
};
int acpi_test_count = sizeof(acpi_tests) / sizeof(acpi_tests[0]);
