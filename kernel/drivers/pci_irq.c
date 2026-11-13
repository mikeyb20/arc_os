/* arc_os — PCI interrupts: MSI-X, MSI, INTx (M1-09, ADR-0006)
 *
 * Owns: the MSI and MSI-X capabilities and table of each PCI function, the
 * INTx disable bit, and the IrqChip ops the IRQ layer (irq.c) programs them
 * through. Drivers never touch these registers.
 * Invariants: an MSI-X entry is masked whenever its address or data changes
 * (the IRQ layer masks first); MSI stays disabled until its message is
 * written, so the device can never write to address 0; while MSI or MSI-X is
 * on, INTx is disabled in the command register.
 * Locks: called under irq_lock (chip ops) or the device-model lock (alloc,
 * free); config access takes the PCI config lock.
 * Cost: allocation O(table entries) at probe; chip ops are 1-3 register writes. */

#include "drivers/pci.h"
#include "drivers/irq.h"
#include "drivers/mmio.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

/* MSI-X capability */
#define MSIX_CTRL          2       /* message control (16-bit) */
#define MSIX_CTRL_ENABLE   0x8000
#define MSIX_CTRL_FMASK    0x4000  /* function mask */
#define MSIX_TABLE         4       /* table offset | BIR */
#define MSIX_BIR_MASK      0x7
#define MSIX_ENTRY_SIZE    16
#define MSIX_ENTRY_ADDR_LO 0
#define MSIX_ENTRY_ADDR_HI 4
#define MSIX_ENTRY_DATA    8
#define MSIX_ENTRY_CTRL    12
#define MSIX_ENTRY_MASKED  0x1

/* MSI capability */
#define MSI_CTRL          2
#define MSI_CTRL_ENABLE   0x0001
#define MSI_CTRL_MME_MASK 0x0070  /* multiple message enable: we use one */
#define MSI_CTRL_64BIT    0x0080
#define MSI_CTRL_PVM      0x0100  /* per-vector masking */
#define MSI_ADDR_LO       4

/* ---- MSI-X ---- */

static volatile uint32_t *msix_entry(IrqDesc *d) {
    PciDevice *p = d->chip_data;
    return (volatile uint32_t *)((volatile uint8_t *)p->msix_table + (uint32_t)d->hwindex * MSIX_ENTRY_SIZE);
}

static void msix_write_msg(IrqDesc *d, uint64_t addr, uint32_t data) {
    volatile uint32_t *e = msix_entry(d);
    mmio_write32(&e[MSIX_ENTRY_ADDR_LO / 4], (uint32_t)addr);
    mmio_write32(&e[MSIX_ENTRY_ADDR_HI / 4], (uint32_t)(addr >> 32));
    mmio_write32(&e[MSIX_ENTRY_DATA / 4], data);
}

static void msix_set_mask(IrqDesc *d, int masked) {
    volatile uint32_t *e = msix_entry(d);
    uint32_t ctrl = mmio_read32(&e[MSIX_ENTRY_CTRL / 4]);
    ctrl = masked ? (ctrl | MSIX_ENTRY_MASKED) : (ctrl & ~MSIX_ENTRY_MASKED);
    mmio_write32(&e[MSIX_ENTRY_CTRL / 4], ctrl);
    (void)mmio_read32(&e[MSIX_ENTRY_CTRL / 4]);   /* flush the posted write */
}

static void msix_mask(IrqDesc *d) {
    msix_set_mask(d, 1);
}

static void msix_unmask(IrqDesc *d) {
    msix_set_mask(d, 0);
}

static const IrqChip msix_chip = {"PCI-MSIX", msix_write_msg, msix_mask, msix_unmask};

/* Map dev's MSI-X table (once). Returns 0 or -ENODEV. */
static int msix_map_table(PciDevice *dev) {
    if (dev->msix_table != NULL) return 0;
    uint32_t loc = pci_read32(dev, (uint8_t)(dev->cap_msix + MSIX_TABLE));
    uint64_t base = pci_bar_phys(dev, (int)(loc & MSIX_BIR_MASK));
    if (base == 0) return -ENODEV;
    dev->msix_table = mmio_map(base + (loc & ~MSIX_BIR_MASK), (uint64_t)dev->msix_entries * MSIX_ENTRY_SIZE);
    return dev->msix_table != NULL ? 0 : -ENODEV;
}

/* Switch MSI-X on with every entry masked. */
static int msix_enable(PciDevice *dev) {
    pci_enable_decode(dev, 1);
    if (msix_map_table(dev) != 0) return -ENODEV;
    uint8_t ctl = (uint8_t)(dev->cap_msix + MSIX_CTRL);
    uint16_t c = pci_read16(dev, ctl);
    pci_write16(dev, ctl, (uint16_t)(c | MSIX_CTRL_ENABLE | MSIX_CTRL_FMASK));
    for (uint16_t i = 0; i < dev->msix_entries; i++) {
        volatile uint32_t *e =
            (volatile uint32_t *)((volatile uint8_t *)dev->msix_table + (uint32_t)i * MSIX_ENTRY_SIZE);
        mmio_write32(&e[MSIX_ENTRY_CTRL / 4], mmio_read32(&e[MSIX_ENTRY_CTRL / 4]) | MSIX_ENTRY_MASKED);
    }
    pci_write16(dev, ctl, (uint16_t)((c | MSIX_CTRL_ENABLE) & ~MSIX_CTRL_FMASK));
    return 0;
}

/* ---- MSI (one message) ---- */

static uint8_t msi_data_offset(const PciDevice *p, uint16_t ctrl) {
    return (uint8_t)(p->cap_msi + ((ctrl & MSI_CTRL_64BIT) ? 0x0C : 0x08));
}

static void msi_write_msg(IrqDesc *d, uint64_t addr, uint32_t data) {
    PciDevice *p = d->chip_data;
    uint16_t ctrl = pci_read16(p, (uint8_t)(p->cap_msi + MSI_CTRL));
    pci_write32(p, (uint8_t)(p->cap_msi + MSI_ADDR_LO), (uint32_t)addr);
    if (ctrl & MSI_CTRL_64BIT) pci_write32(p, (uint8_t)(p->cap_msi + MSI_ADDR_LO + 4), (uint32_t)(addr >> 32));
    pci_write16(p, msi_data_offset(p, ctrl), (uint16_t)data);
}

/* Mask with the per-vector mask bit when the device has one, else by turning
 * MSI off (INTx stays disabled, so nothing else signals meanwhile). */
static void msi_set_mask(IrqDesc *d, int masked) {
    PciDevice *p = d->chip_data;
    uint8_t ctl = (uint8_t)(p->cap_msi + MSI_CTRL);
    uint16_t ctrl = pci_read16(p, ctl);
    if (ctrl & MSI_CTRL_PVM) {
        uint8_t mask_reg = (uint8_t)(msi_data_offset(p, ctrl) + 4);
        uint32_t bits = pci_read32(p, mask_reg);
        pci_write32(p, mask_reg, masked ? (bits | 1u) : (bits & ~1u));
        if (!masked && !(ctrl & MSI_CTRL_ENABLE)) pci_write16(p, ctl, (uint16_t)(ctrl | MSI_CTRL_ENABLE));
        return;
    }
    ctrl = masked ? (uint16_t)(ctrl & ~MSI_CTRL_ENABLE) : (uint16_t)(ctrl | MSI_CTRL_ENABLE);
    pci_write16(p, ctl, ctrl);
}

static void msi_mask(IrqDesc *d) {
    msi_set_mask(d, 1);
}

static void msi_unmask(IrqDesc *d) {
    msi_set_mask(d, 0);
}

static const IrqChip msi_chip = {"PCI-MSI", msi_write_msg, msi_mask, msi_unmask};

/* ---- INTx ---- */

/* The command register's INTx disable bit (PCI 2.3): masks this function only,
 * not the shared line. */
static void set_intx_disable(PciDevice *dev, int disabled) {
    uint16_t cmd = pci_read16(dev, PCI_REG_COMMAND);
    cmd = disabled ? (uint16_t)(cmd | PCI_CMD_INTX_DISABLE) : (uint16_t)(cmd & ~PCI_CMD_INTX_DISABLE);
    pci_write16(dev, PCI_REG_COMMAND, cmd);
}

static void intx_mask(IrqDesc *d) {
    set_intx_disable(d->chip_data, 1);
}

static void intx_unmask(IrqDesc *d) {
    set_intx_disable(d->chip_data, 0);
}

static const IrqChip intx_chip = {"INTx", NULL, intx_mask, intx_unmask};

/* ---- Allocation ---- */

/* Create n descriptors of one kind for dev. Returns n, or -ENOMEM. */
static int make_descs(PciDevice *dev, unsigned n, uint8_t kind, const IrqChip *chip) {
    IrqDesc **v = kmalloc(sizeof(IrqDesc *) * n, GFP_ZERO);
    if (v == NULL) return -ENOMEM;
    for (unsigned i = 0; i < n; i++) {
        v[i] = irq_desc_create(&dev->dev, kind, chip, dev, (uint16_t)i, dev->irq_line);
        if (v[i] == NULL) {
            while (i-- > 0) irq_desc_destroy(v[i]);
            kfree(v);
            return -ENOMEM;
        }
    }
    dev->dev.irqs = v;
    dev->dev.nr_irqs = (uint16_t)n;
    dev->irq_kind = kind;
    return (int)n;
}

int pci_alloc_irq_vectors(PciDevice *dev, unsigned min, unsigned max, unsigned types) {
    if (dev->dev.irqs != NULL) return -EBUSY;
    if (min == 0) min = 1;
    if (max < min) return -EINVAL;

    if (irq_msi_available() && (types & PCI_IRQ_MSIX) && dev->cap_msix != 0 && dev->msix_entries >= min) {
        unsigned n = max < dev->msix_entries ? max : dev->msix_entries;
        if (msix_enable(dev) == 0) {
            int rc = make_descs(dev, n, IRQ_KIND_MSIX, &msix_chip);
            if (rc > 0) {
                set_intx_disable(dev, 1);
                return rc;
            }
            pci_write16(dev, (uint8_t)(dev->cap_msix + MSIX_CTRL),
                        (uint16_t)(pci_read16(dev, (uint8_t)(dev->cap_msix + MSIX_CTRL)) & ~MSIX_CTRL_ENABLE));
            return rc;
        }
    }
    if (irq_msi_available() && (types & PCI_IRQ_MSI) && dev->cap_msi != 0 && min == 1) {
        uint8_t ctl = (uint8_t)(dev->cap_msi + MSI_CTRL);
        pci_write16(dev, ctl, (uint16_t)(pci_read16(dev, ctl) & ~(MSI_CTRL_ENABLE | MSI_CTRL_MME_MASK)));
        int rc = make_descs(dev, 1, IRQ_KIND_MSI, &msi_chip);
        if (rc > 0) set_intx_disable(dev, 1);   /* MSI itself goes on at the first unmask */
        return rc;
    }
    if ((types & PCI_IRQ_INTX) && min == 1 && dev->irq_pin != 0 && dev->irq_line >= 1 && dev->irq_line < 16) {
        set_intx_disable(dev, 1);   /* until irq_request unmasks it */
        return make_descs(dev, 1, IRQ_KIND_INTX, &intx_chip);
    }
    return -ENODEV;
}

void pci_free_irq_vectors(PciDevice *dev) {
    if (dev->dev.irqs == NULL) return;
    for (unsigned i = 0; i < dev->dev.nr_irqs; i++) irq_free(&dev->dev, i);
    if (dev->irq_kind == IRQ_KIND_MSIX) {
        uint8_t ctl = (uint8_t)(dev->cap_msix + MSIX_CTRL);
        pci_write16(dev, ctl, (uint16_t)(pci_read16(dev, ctl) & ~MSIX_CTRL_ENABLE));
    } else if (dev->irq_kind == IRQ_KIND_MSI) {
        uint8_t ctl = (uint8_t)(dev->cap_msi + MSI_CTRL);
        pci_write16(dev, ctl, (uint16_t)(pci_read16(dev, ctl) & ~MSI_CTRL_ENABLE));
    }
    IrqDesc **v = dev->dev.irqs;
    unsigned n = dev->dev.nr_irqs;
    dev->dev.irqs = NULL;
    dev->dev.nr_irqs = 0;
    dev->irq_kind = 0;
    for (unsigned i = 0; i < n; i++) irq_desc_destroy(v[i]);   /* requested ones stay listed (irq.h) */
    kfree(v);
}

int pci_msix_info(PciDevice *dev, uint16_t *entries, uint8_t *bar, uint32_t *offset, uint16_t *masked) {
    if (dev->cap_msix == 0) return -1;
    uint32_t loc = pci_read32(dev, (uint8_t)(dev->cap_msix + MSIX_TABLE));
    *entries = dev->msix_entries;
    *bar = (uint8_t)(loc & MSIX_BIR_MASK);
    *offset = loc & ~MSIX_BIR_MASK;
    *masked = 0;
    /* Read only: never turn decoding on for a device no driver owns */
    if (!(pci_read16(dev, PCI_REG_COMMAND) & PCI_CMD_MEM_SPACE) || msix_map_table(dev) != 0) return -1;
    for (uint16_t i = 0; i < dev->msix_entries; i++) {
        const volatile uint32_t *e =
            (const volatile uint32_t *)((volatile uint8_t *)dev->msix_table + (uint32_t)i * MSIX_ENTRY_SIZE);
        if (mmio_read32(&e[MSIX_ENTRY_CTRL / 4]) & MSIX_ENTRY_MASKED) (*masked)++;
    }
    return 0;
}
