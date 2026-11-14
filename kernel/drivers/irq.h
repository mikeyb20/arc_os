#ifndef ARCHOS_DRIVERS_IRQ_H
#define ARCHOS_DRIVERS_IRQ_H

/* Device interrupts (M1-09, ADR-0006).
 *
 * A bus gives a device its interrupts as descriptors (pci_alloc_irq_vectors:
 * MSI-X, else MSI, else INTx); the driver then attaches a handler to each by
 * index with irq_request. MSI and MSI-X interrupts get their own vector on one
 * CPU (spread over the CPUs that can be MSI targets: APIC ID below 256, no
 * interrupt remapping) and can move with irq_set_affinity, except plain MSI
 * without per-vector masking. INTx is the legacy line the firmware set up,
 * shared, on the boot CPU.
 *
 * Handlers run in hard-IRQ context with interrupts off: they must not sleep or
 * allocate (work-blocks §7), and should only acknowledge the device and wake a
 * thread. They return IRQ_HANDLED, or IRQ_NONE when the device did not raise
 * the interrupt (shared lines).
 *
 * A handler can run on two CPUs at once: after an affinity move, a message
 * already in flight to the old vector still runs it on the old CPU while a new
 * one runs it on the new CPU. Handlers must tolerate that (take their own
 * IRQ-safe lock around shared state, as virtio-net's rx_lock does).
 *
 * Locks: irq_lock (LOCK_LEVEL_IRQ_DESC, IRQ-safe) around request, free,
 * affinity and /proc/interrupts; the interrupt path takes none. Per-CPU counts
 * are written only by their own CPU. */

#include <stdint.h>
#include "drivers/bus.h"

#define IRQ_NONE    0
#define IRQ_HANDLED 1

/* irq_request flags */
#define IRQF_SHARED 0x1   /* INTx: other devices may use the same line */

/* Descriptor kinds */
#define IRQ_KIND_INTX 1
#define IRQ_KIND_MSI  2
#define IRQ_KIND_MSIX 3

typedef int (*irq_handler_fn)(void *arg);

typedef struct IrqDesc IrqDesc;

/* How a bus programs one kind of interrupt source. */
/* IrqChip flags. FIXED_TARGET: mask() is not a real mask (MSI without
 * per-vector masking turns MSI off, and an event meanwhile is lost), so the
 * target never moves after request. */
#define IRQCHIP_FIXED_TARGET 0x1

typedef struct IrqChip {
    const char *name;                                             /* "PCI-MSIX", "PCI-MSI", "INTx" */
    unsigned flags;                                               /* IRQCHIP_* */
    void (*write_msg)(IrqDesc *d, uint64_t addr, uint32_t data);  /* MSI kinds: where it goes */
    void (*mask)(IrqDesc *d);
    void (*unmask)(IrqDesc *d);
} IrqChip;

struct IrqDesc {
    const IrqChip *chip;
    void *chip_data;          /* the bus's own state (the PciDevice) */
    Device *dev;
    uint16_t hwindex;         /* MSI-X table entry, or 0 */
    uint8_t kind;             /* IRQ_KIND_* */
    uint8_t line;             /* INTx: legacy line (0-15) */
    uint8_t was_requested;    /* a handler was attached once: the memory stays (irq_desc_destroy) */
    uint32_t id;              /* number in /proc/interrupts */
    irq_handler_fn handler;   /* NULL until irq_request */
    void *arg;
    const char *name;
    uint32_t cpu;             /* MSI kinds: target CPU index and vector there */
    int vector;               /* < 0: none */
    uint32_t old_cpu;         /* vector left by the last affinity move (in-flight */
    int old_vector;           /* messages still land on it); freed by the next */
    uint64_t *counts;         /* [irq_cpu_slots()], slot c written only by CPU c */
    IrqDesc *next_shared;     /* INTx: next descriptor on the same line */
    IrqDesc *next_all;        /* every descriptor, oldest first */
    IrqDesc *prev_all;
};

/* Loaded on every interrupt: keep it within two cache lines */
_Static_assert(sizeof(IrqDesc) <= 128, "IrqDesc grew past two cache lines");

/* After the other CPUs are online (hal_smp_init): size the per-CPU counts and
 * hook the device vectors. Before it, irq_request fails. */
void irq_init(void);

/* CPU index slots in each descriptor's counts (highest online index + 1). */
uint32_t irq_cpu_slots(void);

/* 1 if MSI and MSI-X can be delivered (the local APIC is in use). */
int irq_msi_available(void);

/* Bus layers only: a new descriptor for dev, masked and without a handler. */
IrqDesc *irq_desc_create(Device *dev, uint8_t kind, const IrqChip *chip, void *chip_data, uint16_t hwindex,
                         uint8_t line);

/* Bus layers only: drop a descriptor whose handler was freed and whose source
 * is masked. One that never had a handler is freed; one that had stays
 * allocated and listed as "(free)", because another CPU may still be inside
 * its handler and nothing waits for that yet (hot-unplug, M4-02, adds the
 * wait). */
void irq_desc_destroy(IrqDesc *d);

/* Attach fn(arg) to interrupt index of dev and unmask it. Returns 0, -EINVAL
 * (no such index), -EBUSY (already requested; or an INTx line someone holds
 * without IRQF_SHARED), -ENOSPC (no vector) or -ENODEV. */
int irq_request(Device *dev, unsigned index, irq_handler_fn fn, void *arg, unsigned flags, const char *name);

/* Mask interrupt index of dev and detach its handler. */
void irq_free(Device *dev, unsigned index);

/* Move MSI or MSI-X interrupt index of dev to CPU index cpu. Returns 0,
 * -EINVAL, -EOPNOTSUPP (INTx, or a chip with IRQCHIP_FIXED_TARGET), -ERANGE
 * (the CPU's APIC ID is above 255) or -ENOSPC. */
int irq_set_affinity(Device *dev, unsigned index, uint32_t cpu);

/* CPU index interrupt index of dev is delivered to, or -1. */
int irq_get_cpu(Device *dev, unsigned index);

/* Interrupts handled for descriptor d on every CPU together. */
uint64_t irq_desc_total(const IrqDesc *d);

/* /proc/interrupts text into buf (NUL-terminated). Returns its length. */
int irq_format_proc(char *buf, int size);

#endif /* ARCHOS_DRIVERS_IRQ_H */
