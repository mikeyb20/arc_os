/* arc_os — Device interrupt layer (M1-09, ADR-0006)
 *
 * Owns: interrupt descriptors (one per MSI-X entry, MSI message or INTx
 * hookup), handler binding, target-CPU choice, affinity moves, the INTx line
 * chains, interrupt counts and /proc/interrupts (with the CPUs' own
 * timer and IPI counts, M1-06). Vectors come from the HAL
 * (per-CPU tables, arch/x86_64/irq_vector.c); the bus programs the source
 * through the descriptor's IrqChip (drivers/pci_irq.c).
 * Invariants: a requested MSI descriptor owns `vector` on `cpu` and at most one
 * retired vector (`old_vector`) that maps to it too, so a message still in
 * flight after a move is handled, not lost; the handler is set before the
 * source is unmasked and the source is masked before the handler is cleared.
 * An INTx line chain only grows at its head, and an unlinked descriptor keeps
 * its next pointer, so the interrupt path may walk it without a lock.
 * A descriptor that ever had a handler is never freed (irq_desc_destroy).
 * Locks: irq_lock (LOCK_LEVEL_IRQ_DESC, IRQ-safe) for every change and for
 * /proc/interrupts; under it the HAL vector lock and the PCI config lock.
 * Cost: the interrupt path is one table load, one call and one count; request
 * and affinity are O(CPUs); create and destroy O(1); /proc/interrupts is
 * O(descriptors x CPUs). */

#include "drivers/irq.h"
#include "arch/hal.h"
#include "mm/kmalloc.h"
#include "proc/spinlock.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

static Spinlock irq_lock = SPINLOCK_INIT(LOCK_LEVEL_IRQ_DESC, "irq descriptors");
static uint32_t cpu_slots;              /* 0 until irq_init */
static uint16_t cpu_load[HAL_MAX_CPUS];   /* MSI vectors aimed at each CPU */
static IrqDesc *all_head, *all_tail;
static uint32_t next_id;
static IrqDesc *line_chain[HAL_IRQ_LINES];
static uint8_t line_shared[HAL_IRQ_LINES];   /* the chain's handlers all allow sharing */
static uint64_t line_unhandled;

uint32_t irq_cpu_slots(void) {
    return cpu_slots;
}

int irq_msi_available(void) {
    return cpu_slots != 0 && hal_irq_msi_available();
}

/* Count one handled interrupt on this CPU. */
static void count_this_cpu(IrqDesc *d) {
    uint32_t cpu = hal_cpu_index();
    if (cpu < cpu_slots) d->counts[cpu]++;
}

/* Device vector entry (HAL): one MSI or MSI-X descriptor. */
static void irq_vector_entry(void *cookie) {
    IrqDesc *d = cookie;
    irq_handler_fn fn = __atomic_load_n(&d->handler, __ATOMIC_ACQUIRE);
    if (fn != NULL && fn(d->arg) == IRQ_HANDLED) count_this_cpu(d);
}

/* Legacy line entry: every handler on the line's chain. */
static void irq_line_entry(InterruptFrame *frame) {
    uint8_t line = (uint8_t)(frame->vector - HAL_IRQ_LINE_BASE);
    int handled = 0;
    for (IrqDesc *d = __atomic_load_n(&line_chain[line], __ATOMIC_ACQUIRE); d != NULL; d = d->next_shared) {
        irq_handler_fn fn = __atomic_load_n(&d->handler, __ATOMIC_ACQUIRE);
        if (fn != NULL && fn(d->arg) == IRQ_HANDLED) {
            count_this_cpu(d);
            handled = 1;
        }
    }
    if (!handled) __atomic_add_fetch(&line_unhandled, 1, __ATOMIC_RELAXED);
}

void irq_init(void) {
    uint32_t slots = 0;
    for (uint32_t c = 0; c < HAL_MAX_CPUS; c++) {
        if (hal_cpu_online(c)) slots = c + 1;
    }
    cpu_slots = slots;
    hal_irq_set_entry(irq_vector_entry);
    kprintf("[IRQ] %u CPU slots, MSI %s\n", slots, hal_irq_msi_available() ? "available" : "unavailable (PIC mode)");
}

IrqDesc *irq_desc_create(Device *dev, uint8_t kind, const IrqChip *chip, void *chip_data, uint16_t hwindex,
                         uint8_t line) {
    if (cpu_slots == 0) return NULL;
    IrqDesc *d = kmalloc(sizeof(IrqDesc), GFP_ZERO);
    uint64_t *counts = kmalloc(sizeof(uint64_t) * cpu_slots, GFP_ZERO);
    if (d == NULL || counts == NULL) {
        kfree(d);
        kfree(counts);
        return NULL;
    }
    d->dev = dev;
    d->kind = kind;
    d->chip = chip;
    d->chip_data = chip_data;
    d->hwindex = hwindex;
    d->line = line;
    d->vector = -1;
    d->old_vector = -1;
    d->counts = counts;

    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    d->id = next_id++;
    d->prev_all = all_tail;
    if (all_tail != NULL) {
        all_tail->next_all = d;
    } else {
        all_head = d;
    }
    all_tail = d;
    spin_unlock_irqrestore(&irq_lock, flags);
    return d;
}

void irq_desc_destroy(IrqDesc *d) {
    if (d == NULL || d->was_requested) return;   /* see irq.h: kept, listed as (free) */
    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    if (d->prev_all != NULL) {
        d->prev_all->next_all = d->next_all;
    } else {
        all_head = d->next_all;
    }
    if (d->next_all != NULL) {
        d->next_all->prev_all = d->prev_all;
    } else {
        all_tail = d->prev_all;
    }
    spin_unlock_irqrestore(&irq_lock, flags);
    kfree(d->counts);
    kfree(d);
}

static IrqDesc *desc_of(Device *dev, unsigned index) {
    if (dev == NULL || dev->irqs == NULL || index >= dev->nr_irqs) return NULL;
    return dev->irqs[index];
}

/* The MSI target with the fewest vectors (ties: lowest index); irq_lock held.
 * Returns cpu_slots when none qualifies. */
static uint32_t pick_cpu(void) {
    uint32_t best = cpu_slots;
    for (uint32_t c = 0; c < cpu_slots; c++) {
        if (!hal_irq_msi_target_ok(c)) continue;
        if (best == cpu_slots || cpu_load[c] < cpu_load[best]) best = c;
    }
    return best;
}

/* Give d a vector on cpu and point the source at it; irq_lock held, source
 * masked. On success the previous vector becomes the retired one. */
static int msi_target(IrqDesc *d, uint32_t cpu) {
    if (!hal_irq_msi_target_ok(cpu)) return -ERANGE;
    int vector = hal_irq_vector_alloc(cpu, d);
    if (vector < 0) return vector;
    uint64_t addr;
    uint32_t data;
    int rc = hal_irq_msi_message(cpu, vector, &addr, &data);
    if (rc != 0) {
        hal_irq_vector_free(cpu, vector);
        return rc;
    }
    d->chip->write_msg(d, addr, data);
    if (d->old_vector >= 0) hal_irq_vector_free(d->old_cpu, d->old_vector);
    d->old_cpu = d->cpu;
    d->old_vector = d->vector;
    if (d->vector >= 0) cpu_load[d->cpu]--;
    d->cpu = cpu;
    d->vector = vector;
    cpu_load[cpu]++;
    return 0;
}

static int request_line(IrqDesc *d, unsigned flags) {
    IrqDesc *head = line_chain[d->line];
    for (IrqDesc *it = head; it != NULL; it = it->next_shared) {
        if (it == d) return -EBUSY;
    }
    if (head != NULL && !(line_shared[d->line] && (flags & IRQF_SHARED))) return -EBUSY;
    isr_handler_t owner = hal_irq_line_handler(d->line);
    if (owner != NULL && owner != irq_line_entry) return -EBUSY;   /* PIT, keyboard, ... */
    if (head == NULL) {
        line_shared[d->line] = (flags & IRQF_SHARED) != 0;
        hal_irq_register(HAL_IRQ_LINE_BASE + d->line, irq_line_entry);
    }
    d->next_shared = head;
    __atomic_store_n(&line_chain[d->line], d, __ATOMIC_RELEASE);
    hal_irq_line_enable(d->line);
    return 0;
}

int irq_request(Device *dev, unsigned index, irq_handler_fn fn, void *arg, unsigned flags, const char *name) {
    IrqDesc *d = desc_of(dev, index);
    if (d == NULL || fn == NULL) return -EINVAL;

    uint64_t irqf;
    spin_lock_irqsave(&irq_lock, &irqf);
    int rc = 0;
    if (d->handler != NULL) {
        rc = -EBUSY;
    } else {
        d->arg = arg;
        d->name = name;
        d->was_requested = 1;
        __atomic_store_n(&d->handler, fn, __ATOMIC_RELEASE);
        if (d->kind == IRQ_KIND_INTX) {
            rc = d->line < HAL_IRQ_LINES ? request_line(d, flags) : -ENODEV;
        } else {
            uint32_t cpu = pick_cpu();
            rc = cpu < cpu_slots ? msi_target(d, cpu) : -ENODEV;
        }
        if (rc == 0) {
            d->chip->unmask(d);
        } else {
            __atomic_store_n(&d->handler, NULL, __ATOMIC_RELEASE);
        }
    }
    spin_unlock_irqrestore(&irq_lock, irqf);
    if (rc != 0) kprintf("[IRQ] %s: %s interrupt %u not set up (%d)\n", dev->name, d->chip->name, index, rc);
    return rc;
}

void irq_free(Device *dev, unsigned index) {
    IrqDesc *d = desc_of(dev, index);
    if (d == NULL) return;
    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    if (d->handler != NULL) {
        d->chip->mask(d);
        if (d->kind == IRQ_KIND_INTX) {
            IrqDesc **pp = &line_chain[d->line];
            while (*pp != NULL && *pp != d) pp = &(*pp)->next_shared;
            if (*pp == d) __atomic_store_n(pp, d->next_shared, __ATOMIC_RELEASE);   /* d->next_shared stays */
        } else {
            if (d->vector >= 0) {
                hal_irq_vector_free(d->cpu, d->vector);
                cpu_load[d->cpu]--;
            }
            if (d->old_vector >= 0) hal_irq_vector_free(d->old_cpu, d->old_vector);
            d->vector = d->old_vector = -1;
        }
        __atomic_store_n(&d->handler, NULL, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&irq_lock, flags);
}

int irq_set_affinity(Device *dev, unsigned index, uint32_t cpu) {
    IrqDesc *d = desc_of(dev, index);
    if (d == NULL || cpu >= cpu_slots) return -EINVAL;
    if (d->kind == IRQ_KIND_INTX || (d->chip->flags & IRQCHIP_FIXED_TARGET)) return -EOPNOTSUPP;
    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    int rc = 0;
    if (d->handler == NULL) {
        rc = -EINVAL;
    } else if (d->cpu != cpu) {
        d->chip->mask(d);
        rc = msi_target(d, cpu);
        d->chip->unmask(d);
    }
    spin_unlock_irqrestore(&irq_lock, flags);
    return rc;
}

int irq_get_cpu(Device *dev, unsigned index) {
    IrqDesc *d = desc_of(dev, index);
    if (d == NULL || d->handler == NULL) return -1;
    return d->kind == IRQ_KIND_INTX ? 0 : (int)d->cpu;
}

uint64_t irq_desc_total(const IrqDesc *d) {
    uint64_t sum = 0;
    for (uint32_t c = 0; c < cpu_slots; c++) sum += __atomic_load_n(&d->counts[c], __ATOMIC_RELAXED);
    return sum;
}

/* Append to buf at *pos (ksnprintf truncates at size). */
#define PROC_APPEND(...)                                                                    \
    do {                                                                                    \
        if (*pos < size) *pos += ksnprintf(buf + *pos, (size_t)(size - *pos), __VA_ARGS__); \
    } while (0)

static void format_desc(char *buf, int size, int *pos, const IrqDesc *d) {
    PROC_APPEND("%4u:", d->id);
    for (uint32_t c = 0; c < cpu_slots; c++) {
        if (hal_cpu_online(c)) PROC_APPEND(" %10lu", __atomic_load_n(&d->counts[c], __ATOMIC_RELAXED));
    }
    PROC_APPEND("  %-8s %s", d->chip->name, d->dev != NULL ? d->dev->name : "-");
    if (d->kind == IRQ_KIND_INTX) {
        PROC_APPEND(" line %u", d->line);
    } else {
        PROC_APPEND(" %u", d->hwindex);
        if (d->vector >= 0) PROC_APPEND(" cpu%u/0x%x", d->cpu, (unsigned)d->vector);
    }
    PROC_APPEND("  %s\n", (d->handler != NULL && d->name != NULL) ? d->name : "(free)");
}

/* The CPUs' own interrupts, as Linux names them (M1-06) */
static const struct {
    const char *tag, *name;
    int which;
} cpu_irqs[] = {
    {"LOC", "Local timer interrupts", HAL_IRQ_STAT_TIMER},
    {"RES", "Rescheduling interrupts", HAL_IRQ_STAT_RESCHED},
    {"CAL", "Function call interrupts", HAL_IRQ_STAT_CALL},
    {"TLB", "TLB shootdowns", HAL_IRQ_STAT_TLB},
};

int irq_format_proc(char *buf, int size) {
    if (size <= 0) return 0;
    buf[0] = '\0';
    int len = 0;
    int *pos = &len;
    PROC_APPEND("     ");
    for (uint32_t c = 0; c < cpu_slots; c++) {
        if (hal_cpu_online(c)) PROC_APPEND("       CPU%-3u", c);
    }
    PROC_APPEND("\n");
    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    for (const IrqDesc *d = all_head; d != NULL; d = d->next_all) format_desc(buf, size, pos, d);
    spin_unlock_irqrestore(&irq_lock, flags);
    for (uint32_t i = 0; i < sizeof(cpu_irqs) / sizeof(cpu_irqs[0]); i++) {
        PROC_APPEND("%4s:", cpu_irqs[i].tag);
        for (uint32_t c = 0; c < cpu_slots; c++) {
            if (hal_cpu_online(c)) PROC_APPEND(" %10lu", hal_cpu_irq_stat(c, cpu_irqs[i].which));
        }
        PROC_APPEND("  %s\n", cpu_irqs[i].name);
    }
    PROC_APPEND(" ERR: %lu\n", hal_irq_spurious_count() + __atomic_load_n(&line_unhandled, __ATOMIC_RELAXED));
    return len < size ? len : size - 1;
}
