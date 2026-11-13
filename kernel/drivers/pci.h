#ifndef ARCHOS_DRIVERS_PCI_H
#define ARCHOS_DRIVERS_PCI_H

/* PCI bus (M0 enumeration; driver model, BARs, capabilities and interrupts
 * from M1-09, ADR-0006). Only this layer and pci_irq.c touch config space. */

#include <stdint.h>
#include "drivers/bus.h"

/* PCI config space I/O ports (Type 1 mechanism) */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

/* Maximum devices tracked during enumeration */
#define PCI_MAX_DEVICES  64

/* PCI config space register offsets */
#define PCI_REG_VENDOR_ID     0x00
#define PCI_REG_DEVICE_ID     0x02
#define PCI_REG_COMMAND       0x04
#define PCI_REG_STATUS        0x06
#define PCI_REG_CLASS         0x08  /* Class/subclass/prog-if/rev */
#define PCI_REG_HEADER_TYPE   0x0E
#define PCI_REG_BAR0          0x10
#define PCI_REG_BAR1          0x14
#define PCI_REG_BAR2          0x18
#define PCI_REG_BAR3          0x1C
#define PCI_REG_BAR4          0x20
#define PCI_REG_BAR5          0x24
#define PCI_REG_SUBSYSTEM     0x2C  /* subsystem vendor (low), subsystem ID (high) */
#define PCI_REG_CAP_PTR       0x34
#define PCI_REG_IRQ_LINE      0x3C
#define PCI_REG_IRQ_PIN       0x3D

/* PCI command register bits */
#define PCI_CMD_IO_SPACE      (1 << 0)
#define PCI_CMD_MEM_SPACE     (1 << 1)
#define PCI_CMD_BUS_MASTER    (1 << 2)
#define PCI_CMD_INTX_DISABLE  (1 << 10)

/* Status register: a capability list exists */
#define PCI_STATUS_CAP_LIST (1 << 4)

/* Capability IDs */
#define PCI_CAP_ID_MSI  0x05
#define PCI_CAP_ID_MSIX 0x11

/* Header type bit 7 = multi-function */
#define PCI_HEADER_MULTIFUNCTION  0x80

/* Invalid vendor ID (slot empty) */
#define PCI_VENDOR_NONE  0xFFFF

/* BAR flag bits */
#define PCI_BAR_IO          0x1
#define PCI_BAR_MEM_TYPE_64 0x4
#define PCI_NUM_BARS        6

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} PciAddress;

typedef struct PciDevice {
    Device dev;                /* driver model (bus.h); dev.name = name */
    PciAddress addr;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor;
    uint16_t subsys_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;          /* firmware's legacy line (0xFF or 0 = none) */
    uint8_t irq_pin;           /* 1-4 = INTA-INTD, 0 = no INTx */
    uint8_t cap_msi;           /* config offset of the MSI capability, 0 = none */
    uint8_t cap_msix;          /* config offset of the MSI-X capability, 0 = none */
    uint8_t irq_kind;          /* IRQ_KIND_* given by pci_alloc_irq_vectors, 0 = none */
    uint16_t msix_entries;     /* MSI-X table size (0 = no MSI-X) */
    uint32_t bar[PCI_NUM_BARS];       /* raw BAR registers as enumerated */
    uint64_t bar_size[PCI_NUM_BARS];  /* bytes decoded; 0 = unused (or upper half of a 64-bit BAR) */
    volatile uint32_t *msix_table;    /* mapped MSI-X table (pci_irq.c), or NULL */
    char name[16];             /* "0000:00:04.0" */
} PciDevice;

/* PCI_MAX_DEVICES of these sit in .bss (kernel_image_bytes budget) */
_Static_assert(sizeof(PciDevice) <= 200, "PciDevice grew: check the kernel image budget");

/* One match-table entry. PCI_ANY_ID matches any value; the class is
 * (class << 16) | (subclass << 8) | prog_if, compared under class_mask
 * (mask 0 = any class). A table ends with an entry whose vendor is 0. */
typedef struct {
    uint16_t vendor, device;
    uint16_t subvendor, subdevice;
    uint32_t class_code, class_mask;
} PciDeviceId;

#define PCI_ANY_ID 0xFFFF

/* Entry for one vendor:device, any subsystem and class */
#define PCI_DEVICE(v, d) \
    { .vendor = (v), .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, .class_code = 0, .class_mask = 0 }
/* Entry for a class (e.g. 0x010802 NVMe with mask 0xFFFFFF) from any vendor */
#define PCI_DEVICE_CLASS(cls, mask)                                                                   \
    {                                                                                                 \
        .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, \
        .class_code = (cls), .class_mask = (mask)                                                     \
    }

/* A PCI driver: the generic driver (name, suspend, resume) plus typed probe
 * and remove and the ID table the bus matches against. */
typedef struct PciDriver {
    Driver driver;
    const PciDeviceId *ids;
    int (*probe)(PciDevice *pdev, const PciDeviceId *id);
    void (*remove)(PciDevice *pdev);
} PciDriver;

static inline PciDevice *to_pci_device(Device *dev) {
    return container_of(dev, PciDevice, dev);
}

/* Config space access, serialized by the PCI config lock (IRQ-safe leaf).
 * offset must be aligned to the access size. */
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_read16(const PciDevice *dev, uint8_t offset);
void pci_write16(const PciDevice *dev, uint8_t offset, uint16_t value);
uint32_t pci_read32(const PciDevice *dev, uint8_t offset);
void pci_write32(const PciDevice *dev, uint8_t offset, uint32_t value);

/* Scan all PCI buses: IDs, class, BARs (sized), capabilities. Boot, BSP. */
void pci_init(void);

/* Register the PCI bus and add every enumerated device to it (after
 * pci_init and irq_init; drivers registered later bind as they come). */
void pci_bus_init(void);

/* Register a PCI driver; it binds to every matching unbound device. Returns
 * how many it bound. */
int pci_register_driver(PciDriver *drv);

/* The first entry of ids that matches dev, or NULL (host-tested). */
const PciDeviceId *pci_match_id(const PciDeviceId *ids, const PciDevice *dev);

/* Config offset of capability cap_id, or 0 (bounded walk). */
uint8_t pci_find_capability(const PciDevice *dev, uint8_t cap_id);

/* Enable bus mastering (DMA) for a device. */
void pci_enable_bus_master(const PciDevice *dev);

/* Turn on memory (mem = 1) or I/O (mem = 0) decoding. */
void pci_enable_decode(const PciDevice *dev, int mem);

/* Extract I/O port base address from a BAR value (clears flag bits). */
uint16_t pci_bar_io_base(uint32_t bar);

/* Physical address of memory BAR i (64-bit BARs joined), or 0 for an I/O or
 * unused BAR. */
uint64_t pci_bar_phys(const PciDevice *dev, int i);

/* Map memory BAR i uncached (mmio.h) with memory decoding on. Returns the
 * mapping, or NULL. Never unmapped. */
volatile void *pci_bar_map(PciDevice *dev, int i);

/* Name of the bound driver, or NULL. */
static inline const char *pci_driver_name(const PciDevice *dev) {
    return device_driver_name(&dev->dev);
}

/* Short name of a PCI class code ("storage", "network", ...; "other"). */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* Devices found after the table was full (not recorded). */
int pci_get_dropped_count(void);

/* Get the number of discovered devices. */
int pci_get_device_count(void);

/* Get a device by index (0..count-1). */
PciDevice *pci_get_device(int index);

/* ---- Interrupts (pci_irq.c) ---- */

#define PCI_IRQ_INTX 0x1
#define PCI_IRQ_MSI  0x2
#define PCI_IRQ_MSIX 0x4
#define PCI_IRQ_ALL  (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)

/* Give dev between min and max interrupts of the first kind in types that
 * can: MSI-X (up to its table size), then MSI (one), then INTx (one). They
 * appear in dev->dev.irqs, masked, for irq_request. Returns how many, or
 * -ENODEV / -ENOMEM / -EBUSY (already allocated). */
int pci_alloc_irq_vectors(PciDevice *dev, unsigned min, unsigned max, unsigned types);

/* Free every handler and descriptor pci_alloc_irq_vectors gave dev and
 * switch MSI / MSI-X off. */
void pci_free_irq_vectors(PciDevice *dev);

/* MSI-X table facts for the boot report: entries, BAR, offset, and how many
 * entries are masked right now (read from the table). Returns 0, or -1 when
 * dev has no MSI-X or its table cannot be mapped. */
int pci_msix_info(PciDevice *dev, uint16_t *entries, uint8_t *bar, uint32_t *offset, uint16_t *masked);

#endif /* ARCHOS_DRIVERS_PCI_H */
