/* arc_os — Host-side tests for kernel/arch/x86_64/idt.c (idt_set_gate only) */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_ARCH_X86_64_ISR_H
#define ARCHOS_ARCH_X86_64_GDT_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Constants from guarded headers */
#define GDT_KERNEL_CODE  0x08
#define ISR_COUNT        256

/* Provide fake isr_stub_table (we never call idt_init) */
static uint64_t isr_stub_table[256];

/* Include idt.c — it pulls in idt.h which defines IDTEntry/IDTPointer.
 * We only test idt_set_gate, never call idt_init. */
#include "../kernel/arch/x86_64/idt.c"

/* --- Tests --- */

TEST(set_gate_basic) {
    uint64_t addr = 0xDEADBEEF12345678ULL;
    idt_set_gate(0, addr, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 0);

    ASSERT_EQ(idt[0].offset_low, 0x5678);
    ASSERT_EQ(idt[0].offset_mid, 0x1234);
    ASSERT_EQ(idt[0].offset_high, 0xDEADBEEF);
    ASSERT_EQ(idt[0].selector, GDT_KERNEL_CODE);
    ASSERT_EQ(idt[0].type_attr, IDT_GATE_INTERRUPT);
    return 0;
}

TEST(set_gate_address_reconstruction) {
    uint64_t addr = 0xDEADBEEF12345678ULL;
    idt_set_gate(1, addr, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 0);

    uint64_t reconstructed = (uint64_t)idt[1].offset_low
                           | ((uint64_t)idt[1].offset_mid << 16)
                           | ((uint64_t)idt[1].offset_high << 32);
    ASSERT_EQ(reconstructed, addr);
    return 0;
}

TEST(set_gate_with_ist) {
    idt_set_gate(8, 0x1000, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 1);
    ASSERT_EQ(idt[8].ist, 1);

    idt_set_gate(9, 0x2000, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 7);
    ASSERT_EQ(idt[9].ist, 7);
    return 0;
}

TEST(set_gate_ist_masked) {
    /* IST is 3 bits (0-7), so 0xFF should be masked to 0x07 */
    idt_set_gate(10, 0x3000, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 0xFF);
    ASSERT_EQ(idt[10].ist, 0x07);
    return 0;
}

TEST(set_gate_trap_type) {
    idt_set_gate(11, 0x4000, GDT_KERNEL_CODE, IDT_GATE_TRAP, 0);
    ASSERT_EQ(idt[11].type_attr, 0x8F);
    return 0;
}

TEST(set_gate_user_interrupt) {
    idt_set_gate(12, 0x5000, GDT_KERNEL_CODE, IDT_GATE_USER_INT, 0);
    ASSERT_EQ(idt[12].type_attr, 0xEE);
    return 0;
}

TEST(set_gate_zero_handler) {
    idt_set_gate(13, 0, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 0);
    ASSERT_EQ(idt[13].offset_low, 0);
    ASSERT_EQ(idt[13].offset_mid, 0);
    ASSERT_EQ(idt[13].offset_high, 0);
    return 0;
}

TEST(set_gate_max_handler) {
    uint64_t addr = 0xFFFFFFFFFFFFFFFFULL;
    idt_set_gate(14, addr, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 0);
    ASSERT_EQ(idt[14].offset_low, 0xFFFF);
    ASSERT_EQ(idt[14].offset_mid, 0xFFFF);
    ASSERT_EQ(idt[14].offset_high, 0xFFFFFFFF);
    return 0;
}

TEST(set_gate_reserved_always_zero) {
    /* Pre-fill with garbage */
    memset(&idt[15], 0xCC, sizeof(IDTEntry));
    idt_set_gate(15, 0x6000, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT, 0);
    ASSERT_EQ(idt[15].reserved, 0);
    return 0;
}

TEST(set_gate_multiple_vectors) {
    /* Set all 256 gates, then spot-check a few */
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, (uint64_t)(i * 0x1000), GDT_KERNEL_CODE,
                     IDT_GATE_INTERRUPT, 0);
    }

    ASSERT_EQ(idt[0].offset_low, 0x0000);
    ASSERT_EQ(idt[1].offset_low, 0x1000);
    ASSERT_EQ(idt[255].offset_low, (uint16_t)(255 * 0x1000));

    /* All selectors should be GDT_KERNEL_CODE */
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ(idt[i].selector, GDT_KERNEL_CODE);
    }
    return 0;
}

TEST(idt_entry_size) {
    ASSERT_EQ(sizeof(IDTEntry), 16);
    return 0;
}

/* --- Test suite export --- */

TestCase idt_tests[] = {
    TEST_ENTRY(set_gate_basic),
    TEST_ENTRY(set_gate_address_reconstruction),
    TEST_ENTRY(set_gate_with_ist),
    TEST_ENTRY(set_gate_ist_masked),
    TEST_ENTRY(set_gate_trap_type),
    TEST_ENTRY(set_gate_user_interrupt),
    TEST_ENTRY(set_gate_zero_handler),
    TEST_ENTRY(set_gate_max_handler),
    TEST_ENTRY(set_gate_reserved_always_zero),
    TEST_ENTRY(set_gate_multiple_vectors),
    TEST_ENTRY(idt_entry_size),
};

int idt_test_count = sizeof(idt_tests) / sizeof(idt_tests[0]);
