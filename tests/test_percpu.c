/* arc_os — Host-side tests for per-CPU blocks (M1-03): IST stack layout and
 * the APIC ID -> CPU index map. (GS itself is checked in QEMU: each AP reads
 * its index back through GS, qemu.smp.*.) */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Guard kernel headers that need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_MM_PMM_H
#define ARCHOS_MM_PAGE_H

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}

/* Page allocator stub: a "page" is the block itself */
struct page;
static int alloc_fail;
static struct page *alloc_pages(unsigned order, uint32_t flags) {
    (void)flags;
    if (alloc_fail) return NULL;
    return (struct page *)aligned_alloc(4096, (size_t)4096 << order);
}
static inline void *page_to_virt(struct page *pg) {
    return pg;
}


#include "../kernel/arch/x86_64/percpu.c"

/* cpu.c is not linked: percpu_load (not run on the host) calls this */
uint8_t cpu_core_type_detect(void) {
    return CPU_CORE_UNKNOWN;
}

static void reset_percpu(void) {
    for (uint32_t i = 1; i < MAX_CPUS; i++) {
        if (percpu_table[i]) free((uint8_t *)percpu_table[i] - AREA_PERCPU_OFFSET);
    }
    memset(percpu_table, 0, sizeof(percpu_table));
    apic_map_count = 0;
    alloc_fail = 0;
    percpu_setup(&bsp_block, bsp_ist_stacks, 0, 0);   /* percpu_init_bsp minus the MSRs */
}

/* --- Tests --- */

TEST(ap_block_layout) {
    reset_percpu();
    PerCpu *c = percpu_alloc(1, 7);
    ASSERT_TRUE(c != NULL);
    uint8_t *area = (uint8_t *)c - AREA_PERCPU_OFFSET;
    ASSERT_TRUE(c->self == c);
    ASSERT_EQ(c->cpu_index, 1);
    ASSERT_EQ(c->apic_id, 7);
    ASSERT_TRUE(c->tss == &c->tss_storage);
    /* Three 4 KiB IST stacks below the block, each top inside the area */
    ASSERT_EQ(c->tss->ist[IST_DOUBLE_FAULT - 1], (uint64_t)(area + 4096));
    ASSERT_EQ(c->tss->ist[IST_NMI - 1], (uint64_t)(area + 8192));
    ASSERT_EQ(c->tss->ist[IST_MACHINE_CHECK - 1], (uint64_t)(area + 12288));
    ASSERT_TRUE((uint8_t *)(c + 1) <= area + PERCPU_AREA_SIZE);
    ASSERT_EQ((uint64_t)c % 64, 0);   /* hot fields on one cache line */
    ASSERT_TRUE(percpu_get(1) == c);
    ASSERT_TRUE(percpu_get(2) == NULL);
    ASSERT_TRUE(percpu_get(MAX_CPUS) == NULL);
    return 0;
}

TEST(alloc_rejects_bad_index) {
    reset_percpu();
    ASSERT_TRUE(percpu_alloc(0, 1) == NULL);          /* the BSP's block is static */
    ASSERT_TRUE(percpu_alloc(MAX_CPUS, 1) == NULL);
    ASSERT_TRUE(percpu_alloc(3, 1) != NULL);
    ASSERT_TRUE(percpu_alloc(3, 2) == NULL);          /* already there */
    alloc_fail = 1;
    ASSERT_TRUE(percpu_alloc(4, 4) == NULL);
    ASSERT_EQ(percpu_index_of_apic(4), -1);
    return 0;
}

TEST(apic_map_sparse_ids) {
    reset_percpu();
    percpu_set_bsp_apic_id(0x40);
    ASSERT_TRUE(percpu_alloc(1, 0x12345) != NULL);
    ASSERT_TRUE(percpu_alloc(2, 0) != NULL);
    ASSERT_TRUE(percpu_alloc(3, 0x100) != NULL);
    ASSERT_TRUE(percpu_alloc(4, 0xFFFFFFFE) != NULL);
    ASSERT_EQ(percpu_index_of_apic(0x40), 0);
    ASSERT_EQ(percpu_index_of_apic(0x12345), 1);
    ASSERT_EQ(percpu_index_of_apic(0), 2);
    ASSERT_EQ(percpu_index_of_apic(0x100), 3);
    ASSERT_EQ(percpu_index_of_apic(0xFFFFFFFE), 4);
    ASSERT_EQ(percpu_index_of_apic(1), -1);
    ASSERT_EQ(percpu_index_of_apic(0xFFFFFFFF), -1);
    ASSERT_EQ(bsp_block.apic_id, 0x40);
    return 0;
}

TEST(apic_map_full) {
    reset_percpu();
    percpu_set_bsp_apic_id(MAX_CPUS * 3);
    for (uint32_t i = 1; i < MAX_CPUS; i++) {
        ASSERT_TRUE(percpu_alloc(i, (MAX_CPUS - i) * 3) != NULL);   /* descending IDs */
    }
    ASSERT_EQ(apic_map_count, MAX_CPUS);
    for (uint32_t i = 1; i < MAX_CPUS; i++) {
        ASSERT_EQ(percpu_index_of_apic((MAX_CPUS - i) * 3), (int)i);
        ASSERT_EQ(percpu_index_of_apic((MAX_CPUS - i) * 3 + 1), -1);
    }
    ASSERT_EQ(percpu_index_of_apic(MAX_CPUS * 3), 0);
    return 0;
}

TEST(describe_needs_online) {
    reset_percpu();
    PerCpu *c = percpu_alloc(1, 9);
    uint32_t apic = 0;
    uint8_t type = 0;
    ASSERT_EQ(cpu_describe(1, &apic, &type), -1);
    c->online = 1;
    c->core_type = CPU_CORE_ATOM;
    ASSERT_EQ(cpu_describe(1, &apic, &type), 0);
    ASSERT_EQ(apic, 9);
    ASSERT_EQ(type, CPU_CORE_ATOM);
    ASSERT_EQ(cpu_describe(MAX_CPUS, &apic, &type), -1);
    return 0;
}

TestCase percpu_tests[] = {
    TEST_ENTRY(ap_block_layout), TEST_ENTRY(alloc_rejects_bad_index), TEST_ENTRY(apic_map_sparse_ids),
    TEST_ENTRY(apic_map_full),   TEST_ENTRY(describe_needs_online),
};
int percpu_test_count = sizeof(percpu_tests) / sizeof(percpu_tests[0]);
