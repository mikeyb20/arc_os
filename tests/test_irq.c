/* arc_os — Host-side tests for kernel/drivers/irq.c (device interrupts, M1-09)
 *
 * The real irq.c runs against a fake HAL (CPUs, per-CPU vector tables, MSI
 * messages, legacy lines) and a fake interrupt chip that records what the
 * device was told. Interrupts are "delivered" by calling the entry points the
 * layer registered, on a chosen simulated CPU. */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Guard the HAL and allocator; the fakes below stand in */
#define ARCHOS_ARCH_HAL_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_KPRINTF_H

#include <stddef.h>
#include "mm/gfp.h"
#include "arch/x86_64/isr.h"
#include "arch/hal_lock.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}
int ksnprintf(char *buf, size_t size, const char *fmt, ...);   /* the real one (test_kprintf.c) */

static void *kmalloc(size_t size, uint32_t flags) {
    return (flags & GFP_ZERO) ? calloc(1, size) : malloc(size);
}

static void kfree(void *p) {
    free(p);
}

/* ---- Fake HAL ---- */

#define HAL_MAX_CPUS      8
#define HAL_IRQ_LINES     16
#define HAL_IRQ_LINE_BASE 32
#define FAKE_VECTORS      4   /* per CPU, so exhaustion is testable */

static int cpu_online[HAL_MAX_CPUS];
static uint32_t cpu_apic[HAL_MAX_CPUS];
static uint32_t this_cpu;
static int msi_ok = 1;
static void (*entry_fn)(void *cookie);
static void *vec_cookie[HAL_MAX_CPUS][FAKE_VECTORS];
static isr_handler_t line_handler[HAL_IRQ_LINES];
static int line_enabled[HAL_IRQ_LINES];

static uint32_t hal_cpu_index(void) {
    return this_cpu;
}

static int hal_cpu_online(uint32_t cpu) {
    return cpu < HAL_MAX_CPUS && cpu_online[cpu];
}

static void hal_irq_set_entry(void (*fn)(void *cookie)) {
    entry_fn = fn;
}

static int hal_irq_vector_alloc(uint32_t cpu, void *cookie) {
    for (int v = 0; v < FAKE_VECTORS; v++) {
        if (vec_cookie[cpu][v] == NULL) {
            vec_cookie[cpu][v] = cookie;
            return 0x30 + v;
        }
    }
    return -28;   /* -ENOSPC */
}

static void hal_irq_vector_free(uint32_t cpu, int vector) {
    vec_cookie[cpu][vector - 0x30] = NULL;
}

static int hal_irq_msi_message(uint32_t cpu, int vector, uint64_t *addr, uint32_t *data) {
    if (cpu_apic[cpu] > 0xFF) return -34;   /* -ERANGE */
    *addr = 0xFEE00000ULL | (cpu_apic[cpu] << 12);
    *data = (uint32_t)vector;
    return 0;
}

static int hal_irq_msi_available(void) {
    return msi_ok;
}

static int hal_irq_msi_target_ok(uint32_t cpu) {
    return hal_cpu_online(cpu) && cpu_apic[cpu] <= 0xFF;
}

static uint64_t hal_irq_spurious_count(void) {
    return 0;
}

static isr_handler_t hal_irq_line_handler(uint8_t line) {
    return line_handler[line];
}

static void hal_irq_register(int vector, isr_handler_t handler) {
    line_handler[vector - HAL_IRQ_LINE_BASE] = handler;
}

static void hal_irq_line_enable(uint8_t line) {
    line_enabled[line] = 1;
}

#include "../kernel/drivers/irq.c"

/* ---- Fake chip and devices ---- */

typedef struct {
    uint64_t addr;
    uint32_t data;
    int masked;
    int writes;
    int pending;   /* the device "raised" its interrupt (shared-line checks) */
    int handled;
} FakeSrc;

static void fake_write_msg(IrqDesc *d, uint64_t addr, uint32_t data) {
    FakeSrc *s = d->chip_data;
    s->addr = addr;
    s->data = data;
    s->writes++;
}

static void fake_mask(IrqDesc *d) {
    ((FakeSrc *)d->chip_data)->masked = 1;
}

static void fake_unmask(IrqDesc *d) {
    ((FakeSrc *)d->chip_data)->masked = 0;
}

static const IrqChip fake_chip = {"FAKE", fake_write_msg, fake_mask, fake_unmask};

static int handler(void *arg) {
    FakeSrc *s = arg;
    if (!s->pending) return IRQ_NONE;
    s->pending = 0;
    s->handled++;
    return IRQ_HANDLED;
}

static void reset(int ncpu) {
    memset(cpu_online, 0, sizeof(cpu_online));
    memset(cpu_apic, 0, sizeof(cpu_apic));
    memset(vec_cookie, 0, sizeof(vec_cookie));
    memset(line_handler, 0, sizeof(line_handler));
    memset(line_enabled, 0, sizeof(line_enabled));
    memset(cpu_load, 0, sizeof(cpu_load));
    memset(line_chain, 0, sizeof(line_chain));
    memset(line_shared, 0, sizeof(line_shared));
    all_head = all_tail = NULL;   /* leaks the previous test's descriptors: fine on the host */
    next_id = 0;
    line_unhandled = 0;
    this_cpu = 0;
    msi_ok = 1;
    for (int c = 0; c < ncpu; c++) {
        cpu_online[c] = 1;
        cpu_apic[c] = (uint32_t)c;
    }
    irq_init();
}

/* A device with n descriptors of one kind, each with its own FakeSrc */
typedef struct {
    Device dev;
    IrqDesc *descs[8];
    FakeSrc src[8];
} FakeDev;

static void make_dev(FakeDev *f, const char *name, int n, uint8_t kind, uint8_t line) {
    memset(f, 0, sizeof(*f));
    f->dev.name = name;
    for (int i = 0; i < n; i++) {
        f->descs[i] = irq_desc_create(&f->dev, kind, &fake_chip, &f->src[i], (uint16_t)i, line);
        f->src[i].masked = 1;
    }
    f->dev.irqs = f->descs;
    f->dev.nr_irqs = (uint16_t)n;
}

/* Deliver the message src was last told, on cpu */
static int deliver_msi(FakeSrc *s, uint32_t cpu) {
    uint32_t c = 0;
    while (c < HAL_MAX_CPUS && cpu_apic[c] != ((s->addr >> 12) & 0xFF)) c++;
    if (c != cpu) return -1;   /* the message must target the CPU the test expects */
    this_cpu = cpu;
    s->pending = 1;
    entry_fn(vec_cookie[cpu][s->data - 0x30]);
    this_cpu = 0;
    return 0;
}

static void deliver_line(uint8_t line) {
    InterruptFrame f;
    memset(&f, 0, sizeof(f));
    f.vector = HAL_IRQ_LINE_BASE + line;
    line_handler[line](&f);
}

/* ---- Tests ---- */

/* Vectors spread over the CPUs; the handler runs and counts on its CPU */
static int test_irq_msi_spread_and_count(void) {
    reset(3);
    FakeDev f;
    make_dev(&f, "dev0", 3, IRQ_KIND_MSIX, 0);
    for (unsigned i = 0; i < 3; i++) {
        ASSERT_EQ(irq_request(&f.dev, i, handler, &f.src[i], 0, "q"), 0);
        ASSERT_FALSE(f.src[i].masked);
        ASSERT_EQ(irq_get_cpu(&f.dev, i), (int)i);   /* least-loaded CPU each time */
    }
    ASSERT_EQ(deliver_msi(&f.src[1], 1), 0);
    ASSERT_EQ(deliver_msi(&f.src[1], 1), 0);
    ASSERT_EQ(f.src[1].handled, 2);
    ASSERT_EQ(f.descs[1]->counts[1], 2);
    ASSERT_EQ(irq_desc_total(f.descs[1]), 2);
    ASSERT_EQ(irq_desc_total(f.descs[0]), 0);
    ASSERT_EQ(irq_request(&f.dev, 0, handler, &f.src[0], 0, "q"), -EBUSY);
    ASSERT_EQ(irq_request(&f.dev, 7, handler, &f.src[0], 0, "q"), -EINVAL);
    return 0;
}

/* CPUs that can't be MSI targets (APIC ID above 255) are skipped */
static int test_irq_msi_skips_high_apic(void) {
    reset(2);
    cpu_apic[0] = 300;
    FakeDev f;
    make_dev(&f, "dev0", 2, IRQ_KIND_MSI, 0);
    ASSERT_EQ(irq_request(&f.dev, 0, handler, &f.src[0], 0, "a"), 0);
    ASSERT_EQ(irq_request(&f.dev, 1, handler, &f.src[1], 0, "b"), 0);
    ASSERT_EQ(irq_get_cpu(&f.dev, 0), 1);
    ASSERT_EQ(irq_get_cpu(&f.dev, 1), 1);
    ASSERT_EQ(irq_set_affinity(&f.dev, 0, 0), -ERANGE);
    ASSERT_EQ(irq_get_cpu(&f.dev, 0), 1);   /* unchanged */
    ASSERT_FALSE(f.src[0].masked);          /* and unmasked again */
    return 0;
}

/* After a move the new CPU gets the interrupt; a message still in flight to
 * the old vector is handled too, until the next move retires it */
static int test_irq_affinity_retired_vector(void) {
    reset(3);
    FakeDev f;
    make_dev(&f, "dev0", 1, IRQ_KIND_MSIX, 0);
    ASSERT_EQ(irq_request(&f.dev, 0, handler, &f.src[0], 0, "q"), 0);
    ASSERT_EQ(irq_get_cpu(&f.dev, 0), 0);
    FakeSrc old = f.src[0];
    ASSERT_EQ(irq_set_affinity(&f.dev, 0, 2), 0);
    ASSERT_EQ(irq_get_cpu(&f.dev, 0), 2);
    ASSERT_FALSE(f.src[0].masked);
    ASSERT_EQ(cpu_load[0], 0);
    ASSERT_EQ(cpu_load[2], 1);
    ASSERT_EQ(deliver_msi(&f.src[0], 2), 0);
    ASSERT_EQ(f.descs[0]->counts[2], 1);
    /* the in-flight message on the old vector still reaches the handler */
    ASSERT_TRUE(vec_cookie[0][old.data - 0x30] == f.descs[0]);
    old.pending = 1;
    f.src[0].pending = 1;
    this_cpu = 0;
    entry_fn(vec_cookie[0][old.data - 0x30]);
    ASSERT_EQ(f.descs[0]->counts[0], 1);
    /* the next move frees it */
    ASSERT_EQ(irq_set_affinity(&f.dev, 0, 1), 0);
    ASSERT_TRUE(vec_cookie[0][old.data - 0x30] == NULL);
    /* moving to the same CPU changes nothing */
    int writes = f.src[0].writes;
    ASSERT_EQ(irq_set_affinity(&f.dev, 0, 1), 0);
    ASSERT_EQ(f.src[0].writes, writes);
    ASSERT_EQ(irq_set_affinity(&f.dev, 0, 99), -EINVAL);
    return 0;
}

/* Free masks the source, gives both vectors back and drops the load */
static int test_irq_free_releases_vectors(void) {
    reset(2);
    FakeDev f;
    make_dev(&f, "dev0", 1, IRQ_KIND_MSIX, 0);
    ASSERT_EQ(irq_request(&f.dev, 0, handler, &f.src[0], 0, "q"), 0);
    ASSERT_EQ(irq_set_affinity(&f.dev, 0, 1), 0);
    irq_free(&f.dev, 0);
    ASSERT_TRUE(f.src[0].masked);
    ASSERT_EQ(irq_get_cpu(&f.dev, 0), -1);
    for (int c = 0; c < 2; c++) {
        ASSERT_EQ(cpu_load[c], 0);
        for (int v = 0; v < FAKE_VECTORS; v++) ASSERT_TRUE(vec_cookie[c][v] == NULL);
    }
    /* it can be requested again */
    ASSERT_EQ(irq_request(&f.dev, 0, handler, &f.src[0], 0, "q"), 0);
    return 0;
}

/* Running out of vectors fails cleanly: no handler left, source stays masked */
static int test_irq_vector_exhaustion(void) {
    reset(1);
    FakeDev f;
    make_dev(&f, "dev0", FAKE_VECTORS + 1, IRQ_KIND_MSIX, 0);
    for (unsigned i = 0; i < FAKE_VECTORS; i++) ASSERT_EQ(irq_request(&f.dev, i, handler, &f.src[i], 0, "q"), 0);
    ASSERT_EQ(irq_request(&f.dev, FAKE_VECTORS, handler, &f.src[FAKE_VECTORS], 0, "q"), -ENOSPC);
    ASSERT_TRUE(f.descs[FAKE_VECTORS]->handler == NULL);
    ASSERT_TRUE(f.src[FAKE_VECTORS].masked);
    ASSERT_EQ(cpu_load[0], FAKE_VECTORS);
    return 0;
}

/* INTx: shared handlers on one line; each claims only its own interrupt */
static int test_irq_intx_shared(void) {
    reset(2);
    FakeDev a, b;
    make_dev(&a, "a", 1, IRQ_KIND_INTX, 11);
    make_dev(&b, "b", 1, IRQ_KIND_INTX, 11);
    ASSERT_EQ(irq_request(&a.dev, 0, handler, &a.src[0], IRQF_SHARED, "a"), 0);
    ASSERT_EQ(irq_request(&b.dev, 0, handler, &b.src[0], IRQF_SHARED, "b"), 0);
    ASSERT_TRUE(line_enabled[11]);
    ASSERT_EQ(irq_get_cpu(&a.dev, 0), 0);
    ASSERT_EQ(irq_set_affinity(&a.dev, 0, 1), -EOPNOTSUPP);
    b.src[0].pending = 1;
    deliver_line(11);
    ASSERT_EQ(a.src[0].handled, 0);
    ASSERT_EQ(b.src[0].handled, 1);
    ASSERT_EQ(irq_desc_total(b.descs[0]), 1);
    deliver_line(11);   /* nobody's */
    ASSERT_EQ(line_unhandled, 1);
    /* after free, the other handler still runs */
    irq_free(&b.dev, 0);
    ASSERT_TRUE(b.src[0].masked);
    a.src[0].pending = 1;
    deliver_line(11);
    ASSERT_EQ(a.src[0].handled, 1);
    return 0;
}

static void keyboard_like(InterruptFrame *f) {
    (void)f;
}

/* A line held without IRQF_SHARED, or by the kernel itself, is refused */
static int test_irq_intx_exclusive(void) {
    reset(1);
    FakeDev a, b, c;
    make_dev(&a, "a", 1, IRQ_KIND_INTX, 10);
    make_dev(&b, "b", 1, IRQ_KIND_INTX, 10);
    make_dev(&c, "c", 1, IRQ_KIND_INTX, 1);
    ASSERT_EQ(irq_request(&a.dev, 0, handler, &a.src[0], 0, "a"), 0);
    ASSERT_EQ(irq_request(&b.dev, 0, handler, &b.src[0], IRQF_SHARED, "b"), -EBUSY);
    ASSERT_TRUE(b.descs[0]->handler == NULL);
    line_handler[1] = keyboard_like;   /* "the keyboard" owns line 1 */
    ASSERT_EQ(irq_request(&c.dev, 0, handler, &c.src[0], IRQF_SHARED, "c"), -EBUSY);
    /* once the exclusive holder frees the line, a shared request works */
    irq_free(&a.dev, 0);
    ASSERT_EQ(irq_request(&b.dev, 0, handler, &b.src[0], IRQF_SHARED, "b"), 0);
    return 0;
}

/* Before irq_init nothing can be created; without the APIC MSI is unavailable */
static int test_irq_before_init(void) {
    reset(1);
    cpu_slots = 0;
    FakeDev f;
    memset(&f, 0, sizeof(f));
    ASSERT_TRUE(irq_desc_create(&f.dev, IRQ_KIND_MSI, &fake_chip, &f.src[0], 0, 0) == NULL);
    ASSERT_FALSE(irq_msi_available());
    reset(1);
    ASSERT_TRUE(irq_msi_available());
    msi_ok = 0;
    ASSERT_FALSE(irq_msi_available());
    return 0;
}

/* A descriptor that never had a handler is unlinked and freed; one that had
 * stays listed as (free) */
static int test_irq_desc_destroy(void) {
    reset(1);
    FakeDev f;
    make_dev(&f, "dev0", 4, IRQ_KIND_MSIX, 0);
    ASSERT_EQ(irq_request(&f.dev, 1, handler, &f.src[1], 0, "q"), 0);
    irq_free(&f.dev, 1);
    irq_desc_destroy(f.descs[2]);   /* middle */
    irq_desc_destroy(f.descs[0]);   /* head */
    irq_desc_destroy(f.descs[1]);   /* had a handler: stays */
    ASSERT_TRUE(all_head == f.descs[1]);
    ASSERT_TRUE(f.descs[1]->next_all == f.descs[3]);
    ASSERT_TRUE(f.descs[3]->prev_all == f.descs[1]);
    irq_desc_destroy(f.descs[3]);   /* tail */
    ASSERT_TRUE(all_tail == f.descs[1]);
    ASSERT_TRUE(f.descs[1]->next_all == NULL);
    ASSERT_TRUE(f.descs[1]->prev_all == NULL);
    return 0;
}

/* /proc/interrupts: a column per online CPU, one line per descriptor */
static int test_irq_proc_format(void) {
    reset(2);
    FakeDev f, g;
    make_dev(&f, "0000:00:04.0", 2, IRQ_KIND_MSIX, 0);
    make_dev(&g, "0000:00:05.0", 1, IRQ_KIND_INTX, 11);
    ASSERT_EQ(irq_request(&f.dev, 0, handler, &f.src[0], 0, "virtio-blk config"), 0);
    ASSERT_EQ(irq_request(&f.dev, 1, handler, &f.src[1], 0, "virtio-blk queue"), 0);
    ASSERT_EQ(irq_request(&g.dev, 0, handler, &g.src[0], IRQF_SHARED, "edu"), 0);
    ASSERT_EQ(deliver_msi(&f.src[1], 1), 0);
    char buf[1024];
    int n = irq_format_proc(buf, sizeof(buf));
    ASSERT_EQ(n, (int)strlen(buf));
    ASSERT_TRUE(strstr(buf, "CPU0") != NULL);
    ASSERT_TRUE(strstr(buf, "CPU1") != NULL);
    ASSERT_TRUE(strstr(buf, "   0:          0          0  FAKE     0000:00:04.0 0 cpu0/0x30  virtio-blk config\n") !=
                NULL);
    ASSERT_TRUE(strstr(buf, "   1:          0          1  FAKE     0000:00:04.0 1 cpu1/0x30  virtio-blk queue\n") !=
                NULL);
    ASSERT_TRUE(strstr(buf, "   2:          0          0  FAKE     0000:00:05.0 line 11  edu\n") != NULL);
    ASSERT_TRUE(strstr(buf, " ERR: 0\n") != NULL);
    /* a small buffer truncates and stays terminated */
    char small[16];
    n = irq_format_proc(small, sizeof(small));
    ASSERT_EQ(n, 15);
    ASSERT_EQ((int)strlen(small), 15);
    irq_free(&f.dev, 0);
    irq_format_proc(buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "0000:00:04.0 0  (free)\n") != NULL);
    return 0;
}

TestCase irq_tests[] = {
    {"irq_msi_spread_and_count", test_irq_msi_spread_and_count},
    {"irq_msi_skips_high_apic", test_irq_msi_skips_high_apic},
    {"irq_affinity_retired_vector", test_irq_affinity_retired_vector},
    {"irq_free_releases_vectors", test_irq_free_releases_vectors},
    {"irq_vector_exhaustion", test_irq_vector_exhaustion},
    {"irq_intx_shared", test_irq_intx_shared},
    {"irq_intx_exclusive", test_irq_intx_exclusive},
    {"irq_before_init", test_irq_before_init},
    {"irq_desc_destroy", test_irq_desc_destroy},
    {"irq_proc_format", test_irq_proc_format},
};

int irq_test_count = sizeof(irq_tests) / sizeof(irq_tests[0]);
