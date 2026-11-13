/* arc_os — Per-CPU device interrupt vectors (M1-09)
 *
 * Owns: which of the vectors 0x30-0xDF are in use on each CPU, the cookie each
 * one carries to the generic IRQ layer (drivers/irq.c), dispatch of those
 * vectors, and the MSI address/data format.
 * Invariants: a CPU's table exists once a vector was first taken on it and is
 * never freed; a slot is NULL (free) or holds the cookie of one interrupt. A
 * slot is written only under vector_lock and read without it by the CPU that
 * takes the interrupt (one pointer, atomic).
 * Locks: vector_lock (LOCK_LEVEL_IRQ_VECTOR, IRQ-safe; never taken in an
 * interrupt). Dispatch takes no lock.
 * Cost: alloc scans one CPU's 176 slots (thread context only); dispatch is one
 * table load. */

#include "arch/x86_64/irq_vector.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/lapic.h"
#include "mm/kmalloc.h"
#include "proc/spinlock.h"
#include "uapi/errno.h"
#include <stddef.h>

typedef struct {
    void *slot[IRQ_VECTOR_COUNT];
} VectorTable;

static VectorTable *tables[MAX_CPUS];
static Spinlock vector_lock = SPINLOCK_INIT(LOCK_LEVEL_IRQ_VECTOR, "irq vectors");
static irq_vector_entry_t entry_fn;
static uint64_t spurious;

void irq_vector_set_entry(irq_vector_entry_t fn) {
    entry_fn = fn;
}

int irq_vector_alloc(uint32_t cpu, void *cookie) {
    PerCpu *c = percpu_get(cpu);
    if (c == NULL || !c->online || cookie == NULL) return -EINVAL;

    VectorTable *fresh = NULL;
    if (__atomic_load_n(&tables[cpu], __ATOMIC_ACQUIRE) == NULL) {
        fresh = kmalloc(sizeof(VectorTable), GFP_ZERO);
        if (fresh == NULL) return -ENOMEM;
    }

    uint64_t flags;
    spin_lock_irqsave(&vector_lock, &flags);
    if (tables[cpu] == NULL) {
        __atomic_store_n(&tables[cpu], fresh, __ATOMIC_RELEASE);
        fresh = NULL;
    }
    VectorTable *t = tables[cpu];
    int vector = -ENOSPC;
    for (int i = 0; i < IRQ_VECTOR_COUNT; i++) {
        if (t->slot[i] == NULL) {
            __atomic_store_n(&t->slot[i], cookie, __ATOMIC_RELEASE);
            vector = IRQ_VECTOR_FIRST + i;
            break;
        }
    }
    spin_unlock_irqrestore(&vector_lock, flags);
    kfree(fresh);   /* another caller installed the table first */
    return vector;
}

void irq_vector_free(uint32_t cpu, int vector) {
    if (cpu >= MAX_CPUS || vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST) return;
    uint64_t flags;
    spin_lock_irqsave(&vector_lock, &flags);
    if (tables[cpu] != NULL) __atomic_store_n(&tables[cpu]->slot[vector - IRQ_VECTOR_FIRST], NULL, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&vector_lock, flags);
}

void irq_vector_dispatch(uint8_t vector) {
    uint32_t cpu = percpu_current_index();
    VectorTable *t = cpu < MAX_CPUS ? __atomic_load_n(&tables[cpu], __ATOMIC_ACQUIRE) : NULL;
    void *cookie = NULL;
    if (t != NULL && vector >= IRQ_VECTOR_FIRST && vector <= IRQ_VECTOR_LAST) {
        cookie = __atomic_load_n(&t->slot[vector - IRQ_VECTOR_FIRST], __ATOMIC_ACQUIRE);
    }
    if (cookie != NULL && entry_fn != NULL) {
        entry_fn(cookie);
    } else {
        __atomic_add_fetch(&spurious, 1, __ATOMIC_RELAXED);
    }
    lapic_eoi();   /* after the handler: edge-triggered, and handlers never switch threads */
}

int irq_msi_compose(uint32_t cpu, int vector, uint64_t *addr, uint32_t *data) {
    PerCpu *c = percpu_get(cpu);
    if (c == NULL || !c->online || vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST) return -EINVAL;
    if (c->apic_id > 0xFF) return -ERANGE;
    *addr = irq_msi_address(c->apic_id);
    *data = irq_msi_data(vector);
    return 0;
}

uint64_t irq_vector_spurious(void) {
    return __atomic_load_n(&spurious, __ATOMIC_RELAXED);
}
