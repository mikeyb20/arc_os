/* arc_os — Host-side tests for kernel/arch/x86_64/gdt.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H   /* Use libc memset */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Track gdt_flush calls */
static int gdt_flush_called;
static uint16_t gdt_flush_code_sel;
static uint16_t gdt_flush_data_sel;
static uint16_t gdt_flush_tss_sel;

/* gdt.h is included by gdt.c — we need the gdt_flush declaration to match.
 * The header declares it as extern, so we define it here. */
#include "../kernel/arch/x86_64/gdt.h"

void gdt_flush(const GDTPointer *gdtr_arg, uint16_t code_sel,
               uint16_t data_sel, uint16_t tss_sel) {
    (void)gdtr_arg;
    gdt_flush_called++;
    gdt_flush_code_sel = code_sel;
    gdt_flush_data_sel = data_sel;
    gdt_flush_tss_sel = tss_sel;
}

/* Include the real implementation */
#include "../kernel/arch/x86_64/gdt.c"

static void reset_gdt_state(void) {
    gdt_flush_called = 0;
    gdt_flush_code_sel = 0;
    gdt_flush_data_sel = 0;
    gdt_flush_tss_sel = 0;
    gdt_init();
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

TEST(tss_ist1_set) {
    reset_gdt_state();
    /* IST1 should point to the top of the double-fault stack */
    ASSERT_EQ(tss.ist1, (uint64_t)(df_stack + sizeof(df_stack)));
    return 0;
}

TEST(tss_iomap_base) {
    reset_gdt_state();
    ASSERT_EQ(tss.iomap_base, sizeof(TSS));
    return 0;
}

TEST(gdtr_values) {
    reset_gdt_state();
    ASSERT_EQ(gdtr.limit, sizeof(gdt) - 1);
    ASSERT_EQ(gdtr.base, (uint64_t)gdt);
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

TEST(set_kernel_stack) {
    reset_gdt_state();
    gdt_set_kernel_stack(0xDEADBEEFCAFEBABE);
    ASSERT_EQ(tss.rsp0, 0xDEADBEEFCAFEBABE);
    return 0;
}

TEST(gdt_entry_count) {
    reset_gdt_state();
    /* 7 entries × 8 bytes = 56 bytes, limit = 56 - 1 = 55 */
    ASSERT_EQ(gdtr.limit, 55);
    return 0;
}

/* --- Test suite export --- */

TestCase gdt_tests[] = {
    TEST_ENTRY(null_descriptor),
    TEST_ENTRY(kernel_code_segment),
    TEST_ENTRY(kernel_data_segment),
    TEST_ENTRY(user_data_segment),
    TEST_ENTRY(user_code_segment),
    TEST_ENTRY(user_data_before_user_code),
    TEST_ENTRY(tss_descriptor),
    TEST_ENTRY(tss_ist1_set),
    TEST_ENTRY(tss_iomap_base),
    TEST_ENTRY(gdtr_values),
    TEST_ENTRY(gdt_flush_called_correct),
    TEST_ENTRY(set_kernel_stack),
    TEST_ENTRY(gdt_entry_count),
};

int gdt_test_count = sizeof(gdt_tests) / sizeof(gdt_tests[0]);
