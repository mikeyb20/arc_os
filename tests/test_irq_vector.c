/* arc_os — Host-side tests for kernel/arch/x86_64/irq_vector.c (M1-09)
 *
 * Per-CPU device vector tables and the MSI message format. CPUs are fake
 * PerCpu blocks (renamed past test_percpu.c's real percpu.c). */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_ARCH_X86_64_LAPIC_H
#include <stddef.h>
#include "mm/gfp.h"

static int allocs;
static void *kmalloc(size_t size, uint32_t flags) {
    allocs++;
    return (flags & GFP_ZERO) ? calloc(1, size) : malloc(size);
}
static void kfree(void *p) {
    free(p);
}

static int eois;
static void lapic_eoi(void) {
    eois++;
}

#define percpu_get           irqvec_test_percpu_get
#define percpu_current_index irqvec_test_percpu_current_index
#include "arch/x86_64/percpu.h"

static PerCpu fake_cpus[4];
static uint32_t cur_cpu;

PerCpu *irqvec_test_percpu_get(uint32_t index) {
    return index < 4 ? &fake_cpus[index] : NULL;
}

uint32_t irqvec_test_percpu_current_index(void) {
    return cur_cpu;
}

#include "../kernel/arch/x86_64/irq_vector.c"

static void *last_cookie;
static int entries;
static void entry(void *cookie) {
    entries++;
    last_cookie = cookie;
}

static void reset(void) {
    memset(fake_cpus, 0, sizeof(fake_cpus));
    for (int i = 0; i < 4; i++) {
        fake_cpus[i].online = 1;
        fake_cpus[i].apic_id = (uint32_t)i * 2;
        free(tables[i]);
        tables[i] = NULL;
    }
    cur_cpu = 0;
    eois = entries = allocs = 0;
    last_cookie = NULL;
    spurious = 0;
    irq_vector_set_entry(entry);
}

static int test_irqvec_alloc_dispatch_free(void) {
    reset();
    int a = 1, b = 2;
    int va = irq_vector_alloc(1, &a);
    int vb = irq_vector_alloc(1, &b);
    ASSERT_EQ(va, IRQ_VECTOR_FIRST);
    ASSERT_EQ(vb, IRQ_VECTOR_FIRST + 1);
    ASSERT_EQ(allocs, 1);   /* one table per CPU, on first use */
    cur_cpu = 1;
    irq_vector_dispatch((uint8_t)vb);
    ASSERT_EQ(entries, 1);
    ASSERT_TRUE(last_cookie == &b);
    ASSERT_EQ(eois, 1);
    /* the same vector on another CPU is not bound */
    cur_cpu = 0;
    irq_vector_dispatch((uint8_t)vb);
    ASSERT_EQ(entries, 1);
    ASSERT_EQ(irq_vector_spurious(), 1);
    ASSERT_EQ(eois, 2);   /* EOI either way */
    irq_vector_free(1, va);
    cur_cpu = 1;
    irq_vector_dispatch((uint8_t)va);
    ASSERT_EQ(irq_vector_spurious(), 2);
    ASSERT_EQ(irq_vector_alloc(1, &a), va);   /* reused */
    irq_vector_free(1, 0x20);    /* outside the range: ignored */
    irq_vector_free(99, va);
    return 0;
}

static int test_irqvec_limits(void) {
    reset();
    int x;
    ASSERT_EQ(irq_vector_alloc(1, NULL), -EINVAL);
    ASSERT_EQ(irq_vector_alloc(9, &x), -EINVAL);
    fake_cpus[2].online = 0;
    ASSERT_EQ(irq_vector_alloc(2, &x), -EINVAL);
    for (int i = 0; i < IRQ_VECTOR_COUNT; i++) ASSERT_EQ(irq_vector_alloc(3, &x), IRQ_VECTOR_FIRST + i);
    ASSERT_EQ(irq_vector_alloc(3, &x), -ENOSPC);
    ASSERT_EQ(IRQ_VECTOR_COUNT, 0xDF - 0x30 + 1);
    return 0;
}

static int test_irqvec_msi_message(void) {
    reset();
    uint64_t addr;
    uint32_t data;
    ASSERT_EQ(irq_msi_compose(3, 0x41, &addr, &data), 0);
    ASSERT_TRUE(addr == 0xFEE06000ULL);   /* APIC ID 6, physical mode */
    ASSERT_EQ(data, 0x41);                /* fixed delivery, edge */
    fake_cpus[3].apic_id = 256;
    ASSERT_EQ(irq_msi_compose(3, 0x41, &addr, &data), -ERANGE);
    ASSERT_EQ(irq_msi_compose(0, 0x2F, &addr, &data), -EINVAL);
    ASSERT_EQ(irq_msi_compose(7, 0x41, &addr, &data), -EINVAL);
    return 0;
}

TestCase irq_vector_tests[] = {
    {"irqvec_alloc_dispatch_free", test_irqvec_alloc_dispatch_free},
    {"irqvec_limits", test_irqvec_limits},
    {"irqvec_msi_message", test_irqvec_msi_message},
};

int irq_vector_test_count = sizeof(irq_vector_tests) / sizeof(irq_vector_tests[0]);
