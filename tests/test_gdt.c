/* arc_os — Host-side tests for kernel/arch/x86_64/gdt.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H   /* Use libc memset */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Track gdt_flush calls (and the GDTR it was given: LGDT copies it) */
static int gdt_flush_called;
static uint16_t gdtr_limit;
static uint64_t gdtr_base;
static uint16_t gdt_flush_code_sel;
static uint16_t gdt_flush_data_sel;
static uint16_t gdt_flush_tss_sel;

/* gdt.h is included by gdt.c — we need the gdt_flush declaration to match.
 * The header declares it as extern, so we define it here. */
#include "../kernel/arch/x86_64/gdt.h"

void gdt_flush(const GDTPointer *gdtr_arg, uint16_t code_sel,
               uint16_t data_sel, uint16_t tss_sel) {
    gdtr_limit = gdtr_arg->limit;
    gdtr_base = gdtr_arg->base;
    gdt_flush_called++;
    gdt_flush_code_sel = code_sel;
    gdt_flush_data_sel = data_sel;
    gdt_flush_tss_sel = tss_sel;
}

/* Include the real implementation */
#include "../kernel/arch/x86_64/gdt.c"

/* One CPU's GDT and TSS, as in its PerCpu block */
static GDTEntry gdt[GDT_ENTRY_COUNT];
static TSS tss;

static void reset_gdt_state(void) {
    gdt_flush_called = 0;
    gdt_flush_code_sel = 0;
    gdt_flush_data_sel = 0;
    gdt_flush_tss_sel = 0;
    memset(gdt, 0xAA, sizeof(gdt));   /* gdt_init_cpu must clear it */
    memset(&tss, 0, sizeof(tss));
    tss.ist[0] = 0x1000;              /* IST pointers belong to the caller */
    tss.ist[2] = 0x3000;
    gdt_init_cpu(gdt, &tss);
}

/* --- Tests --- */

TEST(null_descriptor) {
    reset_gdt_state();
    ASSERT_EQ(gdt[0].limit_low, 0);
    ASSERT_EQ(gdt[0].base_low, 0);
    ASSERT_EQ(gdt[0].base_mid, 0);
    ASSERT_EQ(gdt[0].access, 0);
    ASSERT_EQ(gdt[0].granularity, 0);
    ASSERT_EQ(gdt[0].base_high, 0);
    return 0;
}

TEST(kernel_code_segment) {
    reset_gdt_state();
    /* Entry 1 (0x08): access=0x9A (Present|DPL0|Code|Exec|Read), flags=Long(0x2) */
    ASSERT_EQ(gdt[1].access, 0x9A);
    ASSERT_EQ(gdt[1].granularity, 0x2F);  /* flags=0x2 << 4 | limit_high=0x0F */
    ASSERT_EQ(gdt[1].limit_low, 0xFFFF);
    return 0;
}

TEST(kernel_data_segment) {
    reset_gdt_state();
    /* Entry 2 (0x10): access=0x92 (Present|DPL0|Data|Write), flags=0 */
    ASSERT_EQ(gdt[2].access, 0x92);
    ASSERT_EQ(gdt[2].granularity, 0x0F);  /* flags=0x0 << 4 | limit_high=0x0F */
    return 0;
}

TEST(user_data_segment) {
    reset_gdt_state();
    /* Entry 3 (0x18): access=0xF2 (Present|DPL3|Data|Write), flags=0 */
    ASSERT_EQ(gdt[3].access, 0xF2);
    ASSERT_EQ(gdt[3].granularity, 0x0F);
    return 0;
}

TEST(user_code_segment) {
    reset_gdt_state();
    /* Entry 4 (0x20): access=0xFA (Present|DPL3|Code|Exec|Read), flags=Long(0x2) */
    ASSERT_EQ(gdt[4].access, 0xFA);
    ASSERT_EQ(gdt[4].granularity, 0x2F);
    return 0;
}

TEST(user_data_before_user_code) {
    reset_gdt_state();
    /* SYSRET ordering: DPL3 data at idx 3 (0x18), code at idx 4 (0x20) */
    ASSERT_EQ(gdt[3].access, 0xF2);  /* User data */
    ASSERT_EQ(gdt[4].access, 0xFA);  /* User code */
    return 0;
}

TEST(tss_descriptor) {
    reset_gdt_state();
    /* TSS at entry 5-6: access byte should be 0x89 (Present, 64-bit TSS available) */
    TSSDescriptor *desc = (TSSDescriptor *)&gdt[5];
    ASSERT_EQ(desc->access, 0x89);

    /* Reconstruct base from the descriptor fields */
    uint64_t base = (uint64_t)desc->base_low
                  | ((uint64_t)desc->base_mid << 16)
                  | ((uint64_t)desc->base_high << 24)
                  | ((uint64_t)desc->base_upper << 32);
    ASSERT_EQ(base, (uint64_t)&tss);
    return 0;
}

TEST(tss_ist_kept) {
    reset_gdt_state();
    /* gdt_init_cpu leaves the caller's IST stack pointers alone */
    ASSERT_EQ(tss.ist[0], 0x1000);
    ASSERT_EQ(tss.ist[2], 0x3000);
    return 0;
}

TEST(tss_iomap_base) {
    reset_gdt_state();
    ASSERT_EQ(tss.iomap_base, sizeof(TSS));
    return 0;
}

TEST(gdtr_values) {
    reset_gdt_state();
    ASSERT_EQ(gdtr_limit, sizeof(gdt) - 1);
    ASSERT_EQ(gdtr_base, (uint64_t)gdt);
    return 0;
}

TEST(gdt_flush_called_correct) {
    reset_gdt_state();
    ASSERT_EQ(gdt_flush_called, 1);
    ASSERT_EQ(gdt_flush_code_sel, 0x08);
    ASSERT_EQ(gdt_flush_data_sel, 0x10);
    ASSERT_EQ(gdt_flush_tss_sel, 0x28);
    return 0;
}

TEST(per_cpu_tss) {
    /* M1-03: each CPU's GDT points at its own TSS (LTR marks it busy) */
    static GDTEntry gdt2[GDT_ENTRY_COUNT];
    static TSS tss2;
    reset_gdt_state();
    gdt_init_cpu(gdt2, &tss2);
    TSSDescriptor *d1 = (TSSDescriptor *)&gdt[5];
    TSSDescriptor *d2 = (TSSDescriptor *)&gdt2[5];
    uint64_t b1 = d1->base_low | ((uint64_t)d1->base_mid << 16) | ((uint64_t)d1->base_high << 24) |
                  ((uint64_t)d1->base_upper << 32);
    uint64_t b2 = d2->base_low | ((uint64_t)d2->base_mid << 16) | ((uint64_t)d2->base_high << 24) |
                  ((uint64_t)d2->base_upper << 32);
    ASSERT_EQ(b1, (uint64_t)&tss);
    ASSERT_EQ(b2, (uint64_t)&tss2);
    ASSERT_EQ(gdtr_base, (uint64_t)gdt2);
    return 0;
}

TEST(gdt_entry_count) {
    reset_gdt_state();
    /* 7 entries × 8 bytes = 56 bytes, limit = 56 - 1 = 55 */
    ASSERT_EQ(gdtr_limit, 55);
    return 0;
}

/* --- Test suite export --- */

TestCase gdt_tests[] = {
    TEST_ENTRY(null_descriptor),          TEST_ENTRY(kernel_code_segment),
    TEST_ENTRY(kernel_data_segment),      TEST_ENTRY(user_data_segment),
    TEST_ENTRY(user_code_segment),        TEST_ENTRY(user_data_before_user_code),
    TEST_ENTRY(tss_descriptor),           TEST_ENTRY(tss_ist_kept),
    TEST_ENTRY(tss_iomap_base),           TEST_ENTRY(gdtr_values),
    TEST_ENTRY(gdt_flush_called_correct), TEST_ENTRY(per_cpu_tss),
    TEST_ENTRY(gdt_entry_count),
};

int gdt_test_count = sizeof(gdt_tests) / sizeof(gdt_tests[0]);
