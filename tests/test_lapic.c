/* arc_os — Host-side tests for the local APIC access layer (M1-03):
 * xAPIC (MMIO) and x2APIC (MSR) modes, chosen on the BSP and followed by APs. */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>

/* Guard kernel headers that need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_MM_VMM_H
#define ARCHOS_ARCH_X86_64_TSC_H
#define ARCHOS_ARCH_X86_64_ISR_H

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}
static uint64_t vmm_get_hhdm_offset(void) {
    return 0;
}   /* MMIO "phys" = host address */
/* tsc.h / isr.h stubs: the APIC timer's calibration and vector */
typedef struct {
    uint64_t khz;
} TscInfo;
static TscInfo fake_tsc;
static const TscInfo *tsc_info(void) {
    return &fake_tsc;
}
static uint64_t tsc_ns_to_cycles(uint64_t ns) {
    return ns * fake_tsc.khz / 1000000;
}
typedef struct InterruptFrame InterruptFrame;
typedef void (*isr_handler_t)(InterruptFrame *frame);
static int registered_vec;
static isr_handler_t registered_handler;
static void isr_register_handler(int vec, isr_handler_t fn) {
    registered_vec = vec;
    registered_handler = fn;
}

/* Real constants, then fake MSRs and CPUID in place of the instructions */
#include "../kernel/arch/x86_64/msr.h"
#include "../kernel/arch/x86_64/cpuid.h"

#define FAKE_MSRS 64
static struct {
    uint32_t msr;
    uint64_t val;
} fake_msr[FAKE_MSRS];
static int fake_msr_count;
static int msr_writes;
static uint32_t last_msr_written;
static uint32_t fake_cpuid1_ecx;

static uint64_t *fake_msr_slot(uint32_t msr) {
    for (int i = 0; i < fake_msr_count; i++) {
        if (fake_msr[i].msr == msr) return &fake_msr[i].val;
    }
    fake_msr[fake_msr_count].msr = msr;
    fake_msr[fake_msr_count].val = 0;
    return &fake_msr[fake_msr_count++].val;
}
static uint64_t test_rdmsr(uint32_t msr) {
    return *fake_msr_slot(msr);
}
static void test_wrmsr(uint32_t msr, uint64_t v) {
    *fake_msr_slot(msr) = v;
    msr_writes++;
    last_msr_written = msr;
}
static void test_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    (void)sub;
    *a = *b = *d = 0;
    *c = leaf == 1 ? fake_cpuid1_ecx : 0;
}
#define rdmsr test_rdmsr
#define wrmsr test_wrmsr
#define cpuid test_cpuid

#include "../kernel/arch/x86_64/lapic.c"

#define X2APIC_MSR(reg) (MSR_X2APIC_FIRST + ((reg) >> 4))
static uint32_t fake_mmio[1024];   /* one xAPIC page */

static void reset_apic(uint32_t cpuid_ecx, uint64_t apic_base) {
    fake_msr_count = 0;
    msr_writes = 0;
    memset(fake_mmio, 0, sizeof(fake_mmio));
    fake_cpuid1_ecx = cpuid_ecx;
    *fake_msr_slot(MSR_APIC_BASE) = apic_base;
    x2apic_mode = 0;
    lapic_mmio = NULL;
}

#define BASE_XAPIC (0xFEE00000ULL | APIC_BASE_BSP | APIC_BASE_EN)

/* --- Tests --- */

TEST(x2apic_chosen_when_cpuid_has_it) {
    reset_apic(CPUID1_ECX_X2APIC, BASE_XAPIC);
    lapic_init_bsp(0xFEE00000);
    ASSERT_EQ(lapic_x2apic(), 1);
    ASSERT_EQ(*fake_msr_slot(MSR_APIC_BASE), BASE_XAPIC | APIC_BASE_EXTD);
    ASSERT_EQ(*fake_msr_slot(X2APIC_MSR(LAPIC_SVR)), LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);
    ASSERT_TRUE(lapic_mmio == NULL);   /* no MMIO in x2APIC mode */
    return 0;
}

TEST(x2apic_kept_when_firmware_enabled_it) {
    /* EXTD already set: stay in x2APIC even if CPUID were to hide it */
    reset_apic(0, BASE_XAPIC | APIC_BASE_EXTD);
    lapic_init_bsp(0xFEE00000);
    ASSERT_EQ(lapic_x2apic(), 1);
    ASSERT_EQ(*fake_msr_slot(MSR_APIC_BASE), BASE_XAPIC | APIC_BASE_EXTD);
    return 0;
}

TEST(x2apic_id_is_32_bit) {
    reset_apic(CPUID1_ECX_X2APIC, BASE_XAPIC);
    lapic_init_bsp(0);
    *fake_msr_slot(X2APIC_MSR(LAPIC_ID)) = 0x12345;
    ASSERT_EQ(lapic_id(), 0x12345);
    return 0;
}

TEST(x2apic_ipi_one_msr_write) {
    reset_apic(CPUID1_ECX_X2APIC, BASE_XAPIC);
    lapic_init_bsp(0);
    msr_writes = 0;
    lapic_send_ipi(0x12345, 0xF1);
    ASSERT_EQ(msr_writes, 1);
    ASSERT_EQ(last_msr_written, X2APIC_MSR(LAPIC_ICR_LOW));
    ASSERT_EQ(*fake_msr_slot(X2APIC_MSR(LAPIC_ICR_LOW)), (0x12345ULL << 32) | 0xF1 | ICR_LEVEL_ASSERT);
    lapic_send_ipi_all_excluding_self(0xF0);
    ASSERT_EQ(*fake_msr_slot(X2APIC_MSR(LAPIC_ICR_LOW)), 0xF0ULL | ICR_DEST_ALL_EX | ICR_LEVEL_ASSERT);
    return 0;
}

TEST(x2apic_eoi_msr) {
    reset_apic(CPUID1_ECX_X2APIC, BASE_XAPIC);
    lapic_init_bsp(0);
    *fake_msr_slot(X2APIC_MSR(LAPIC_EOI)) = 0xDEAD;
    lapic_eoi();
    ASSERT_EQ(last_msr_written, X2APIC_MSR(LAPIC_EOI));
    ASSERT_EQ(*fake_msr_slot(X2APIC_MSR(LAPIC_EOI)), 0);
    return 0;
}

TEST(ap_follows_bsp_into_x2apic) {
    reset_apic(CPUID1_ECX_X2APIC, BASE_XAPIC);
    lapic_init_bsp(0);
    /* The AP's own IA32_APIC_BASE: still xAPIC (bootloader didn't switch it) */
    *fake_msr_slot(MSR_APIC_BASE) = 0xFEE00000ULL | APIC_BASE_EN;
    lapic_init_ap();
    ASSERT_EQ(*fake_msr_slot(MSR_APIC_BASE), 0xFEE00000ULL | APIC_BASE_EN | APIC_BASE_EXTD);
    return 0;
}

TEST(xapic_mmio_mode) {
    reset_apic(0, BASE_XAPIC);
    lapic_init_bsp((uint64_t)(uintptr_t)fake_mmio);
    ASSERT_EQ(lapic_x2apic(), 0);
    ASSERT_EQ(*fake_msr_slot(MSR_APIC_BASE), BASE_XAPIC);   /* unchanged */
    ASSERT_EQ(fake_mmio[LAPIC_SVR / 4], LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);
    fake_mmio[LAPIC_ID / 4] = 3u << 24;
    ASSERT_EQ(lapic_id(), 3);
    lapic_send_ipi(5, 0xF1);
    ASSERT_EQ(fake_mmio[LAPIC_ICR_HIGH / 4], 5u << 24);
    ASSERT_EQ(fake_mmio[LAPIC_ICR_LOW / 4], 0xF1u | ICR_LEVEL_ASSERT);
    fake_mmio[LAPIC_EOI / 4] = 0xDEAD;
    lapic_eoi();
    ASSERT_EQ(fake_mmio[LAPIC_EOI / 4], 0);
    return 0;
}

TEST(xapic_enables_disabled_apic) {
    reset_apic(0, 0xFEE00000ULL | APIC_BASE_BSP);
    lapic_init_bsp((uint64_t)(uintptr_t)fake_mmio);
    ASSERT_EQ(*fake_msr_slot(MSR_APIC_BASE), BASE_XAPIC);
    return 0;
}

/* One-shot calibration (M1-06 review): the try with the tightest edges wins,
 * so a stall inside one edge can't inflate the APIC rate */
TEST(lapic_best_ratio_skips_stalled_edges) {
    /* 62.5 MHz APIC vs a 2.3 GHz TSC: 625000 ticks per 23000000 cycles */
    LapicSample s[3] = {
        /* stalled between the ICR write and t0's partner: ticks without cycles */
        {.t0 = 1000, .t1 = 400000, .t2 = 23001000, .t3 = 23001100, .ticks = 635000},
        {.t0 = 1000, .t1 = 1100, .t2 = 23001100, .t3 = 23001200, .ticks = 625000},
        /* stalled after the CCR read */
        {.t0 = 1000, .t1 = 1100, .t2 = 23001100, .t3 = 23900000, .ticks = 625000},
    };
    uint64_t want = ((uint64_t)625000 << 32) / 23000100; /* midpoint to midpoint */
    uint64_t got = lapic_best_ratio(s, 3);
    ASSERT_TRUE(got >= want - 2 && got <= want + 2);
    /* Nothing counted (a dead timer): 0, so the caller falls back to a guess */
    s[0].ticks = s[1].ticks = s[2].ticks = 0;
    ASSERT_EQ(lapic_best_ratio(s, 3), 0);
    return 0;
}

TestCase lapic_tests[] = {
    TEST_ENTRY(x2apic_chosen_when_cpuid_has_it),
    TEST_ENTRY(x2apic_kept_when_firmware_enabled_it),
    TEST_ENTRY(x2apic_id_is_32_bit),
    TEST_ENTRY(x2apic_ipi_one_msr_write),
    TEST_ENTRY(x2apic_eoi_msr),
    TEST_ENTRY(ap_follows_bsp_into_x2apic),
    TEST_ENTRY(xapic_mmio_mode),
    TEST_ENTRY(xapic_enables_disabled_apic),
    TEST_ENTRY(lapic_best_ratio_skips_stalled_edges),
};
int lapic_test_count = sizeof(lapic_tests) / sizeof(lapic_tests[0]);
