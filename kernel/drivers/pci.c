/* arc_os — PCI bus: enumeration, config space, BARs, capabilities, binding
 *
 * Owns: the table of enumerated PCI functions (fixed array, filled once at
 * boot), config-space access, BAR sizing, the capability walk, ID-table
 * matching and the PCI bus of the driver model (bus.h). MSI, MSI-X and INTx
 * are in pci_irq.c.
 * Invariants: the device table is written only by pci_init, before any other
 * CPU or driver runs; BAR sizes are measured once there, with decoding off.
 * Each device's Device (dev) is on the PCI bus after pci_bus_init.
 * Locks: pci_config_lock (LOCK_LEVEL_PCI_CONFIG, IRQ-safe leaf) around every
 * address/data port pair; bus lists under the device-model lock (bus.c).
 * Cost: enumeration O(256 buses x 32 slots) once; matching O(table) per
 * (device, driver) pair at registration. */

#include "drivers/pci.h"
#include "drivers/mmio.h"
#include "arch/hal.h"
#include "proc/spinlock.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include <stddef.h>

/* PCI config address construction */
#define PCI_ENABLE_BIT          (1U << 31)
#define PCI_DEVICE_MASK         0x1F
#define PCI_FUNCTION_MASK       0x07
#define PCI_OFFSET_MASK         0xFC

/* Header and BAR parsing */
#define PCI_HEADER_TYPE_MASK    0x7F
#define PCI_BAR_STRIDE          4
#define PCI_BAR_IO_MASK         0xFFFC
#define PCI_BAR_MEM_MASK        0xFFFFFFF0u

/* Word extraction from 32-bit PCI config registers */
#define PCI_WORD0_MASK   0xFFFF
#define PCI_WORD1_SHIFT  16

/* Bus scan limits */
#define PCI_MAX_BUSES           256
#define PCI_MAX_DEV_PER_BUS     32
#define PCI_MAX_FUNCTIONS       8

/* Capability list: at most this many entries are followed (48 fit in config space) */
#define PCI_MAX_CAPS 48

/* Vendor ID indicating absent device */
#define PCI_VENDOR_INVALID      0x0000

static Spinlock pci_config_lock = SPINLOCK_INIT(LOCK_LEVEL_PCI_CONFIG, "pci config");
static PciDevice devices[PCI_MAX_DEVICES];
static int device_count;
static int devices_dropped;   /* found after the table filled up */

static inline uint32_t pci_config_addr(uint8_t bus, uint8_t device,
                                        uint8_t func, uint8_t offset) {
    return PCI_ENABLE_BIT
         | ((uint32_t)bus << 16)
         | ((uint32_t)(device & PCI_DEVICE_MASK) << 11)
         | ((uint32_t)(func & PCI_FUNCTION_MASK) << 8)
         | (offset & PCI_OFFSET_MASK);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, device, func, offset));
    uint32_t v = inl(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&pci_config_lock, flags);
    return v;
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, device, func, offset));
    outl(PCI_CONFIG_DATA, value);
    spin_unlock_irqrestore(&pci_config_lock, flags);
}

uint32_t pci_read32(const PciDevice *d, uint8_t offset) {
    return pci_config_read32(d->addr.bus, d->addr.device, d->addr.function, offset);
}

void pci_write32(const PciDevice *d, uint8_t offset, uint32_t value) {
    pci_config_write32(d->addr.bus, d->addr.device, d->addr.function, offset, value);
}

/* 16-bit accesses use the matching half of the data port, so a write never
 * touches the neighbouring register (the status register's bits clear when
 * written with 1). */
uint16_t pci_read16(const PciDevice *d, uint8_t offset) {
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    outl(PCI_CONFIG_ADDR, pci_config_addr(d->addr.bus, d->addr.device, d->addr.function, offset));
    uint16_t v = inw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)));
    spin_unlock_irqrestore(&pci_config_lock, flags);
    return v;
}

void pci_write16(const PciDevice *d, uint8_t offset, uint16_t value) {
    uint64_t flags;
    spin_lock_irqsave(&pci_config_lock, &flags);
    outl(PCI_CONFIG_ADDR, pci_config_addr(d->addr.bus, d->addr.device, d->addr.function, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
    spin_unlock_irqrestore(&pci_config_lock, flags);
}

/* Extract a single byte (0-3) from a 32-bit PCI config register. */
static inline uint8_t pci_reg_byte(uint32_t reg, int byte) {
    return (uint8_t)((reg >> (byte * 8)) & 0xFF);
}

/* Size of the BAR whose register reads back `probe` after all ones were
 * written (flag bits masked off). */
static uint64_t bar_size_from_probe(uint64_t probe) {
    return probe == 0 ? 0 : (~probe + 1);
}

/* Measure every BAR of d: write all ones with decoding off, read the size
 * mask back, restore. 64-bit memory BARs take two registers; the upper one
 * keeps size 0. Boot only, before drivers run. */
static void pci_size_bars(PciDevice *d) {
    uint16_t cmd = pci_read16(d, PCI_REG_COMMAND);
    pci_write16(d, PCI_REG_COMMAND, (uint16_t)(cmd & ~(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE)));
    for (int i = 0; i < PCI_NUM_BARS; i++) {
        uint8_t reg = (uint8_t)(PCI_REG_BAR0 + i * PCI_BAR_STRIDE);
        uint32_t orig = d->bar[i];
        pci_write32(d, reg, 0xFFFFFFFF);
        uint32_t lo = pci_read32(d, reg);
        pci_write32(d, reg, orig);
        if (lo == 0 || lo == 0xFFFFFFFF) continue;   /* unused, or nothing answered */
        if (orig & PCI_BAR_IO) {
            d->bar_size[i] = bar_size_from_probe((lo & PCI_BAR_IO_MASK) | 0xFFFF0000u) & 0xFFFF;
            continue;
        }
        uint64_t mask = lo & PCI_BAR_MEM_MASK;
        if ((orig & 0x6) == PCI_BAR_MEM_TYPE_64 && i + 1 < PCI_NUM_BARS) {
            uint8_t reg_hi = (uint8_t)(reg + PCI_BAR_STRIDE);
            uint32_t orig_hi = d->bar[i + 1];
            pci_write32(d, reg_hi, 0xFFFFFFFF);
            uint32_t hi = pci_read32(d, reg_hi);
            pci_write32(d, reg_hi, orig_hi);
            mask |= (uint64_t)hi << 32;
            d->bar_size[i] = bar_size_from_probe(mask);
            i++;   /* the upper half is not a BAR of its own */
            continue;
        }
        d->bar_size[i] = bar_size_from_probe(mask | 0xFFFFFFFF00000000ULL);
    }
    pci_write16(d, PCI_REG_COMMAND, cmd);
}

/* Populate a PciDevice entry. Caller already verified vendor != 0xFFFF. */
static void pci_populate_device(uint8_t bus, uint8_t dev, uint8_t func,
                                uint16_t vendor, uint16_t devid) {
    if (device_count >= PCI_MAX_DEVICES) {
        devices_dropped++;
        return;
    }

    PciDevice *d = &devices[device_count];
    memset(d, 0, sizeof(*d));
    d->addr.bus      = bus;
    d->addr.device   = dev;
    d->addr.function = func;
    d->vendor_id     = vendor;
    d->device_id = devid;

    uint32_t class_reg = pci_config_read32(bus, dev, func, PCI_REG_CLASS);
    d->class_code = pci_reg_byte(class_reg, 3);
    d->subclass   = pci_reg_byte(class_reg, 2);
    d->prog_if    = pci_reg_byte(class_reg, 1);
    d->revision   = pci_reg_byte(class_reg, 0);

    /* Header type is byte 2 of the dword at offset 0x0C */
    d->header_type = pci_reg_byte(pci_config_read32(bus, dev, func, PCI_REG_HEADER_TYPE), 2);

    /* BARs and subsystem IDs exist only in header type 0 (normal devices) */
    if ((d->header_type & PCI_HEADER_TYPE_MASK) == 0) {
        for (int i = 0; i < PCI_NUM_BARS; i++) {
            d->bar[i] = pci_config_read32(bus, dev, func, PCI_REG_BAR0 + i * PCI_BAR_STRIDE);
        }
        pci_size_bars(d);
        uint32_t sub = pci_config_read32(bus, dev, func, PCI_REG_SUBSYSTEM);
        d->subsys_vendor = (uint16_t)(sub & PCI_WORD0_MASK);
        d->subsys_id = (uint16_t)(sub >> PCI_WORD1_SHIFT);
    }

    uint32_t irq_reg = pci_config_read32(bus, dev, func, PCI_REG_IRQ_LINE);
    d->irq_line = pci_reg_byte(irq_reg, 0);
    d->irq_pin  = pci_reg_byte(irq_reg, 1);

    d->cap_msi = pci_find_capability(d, PCI_CAP_ID_MSI);
    d->cap_msix = pci_find_capability(d, PCI_CAP_ID_MSIX);
    if (d->cap_msix != 0) d->msix_entries = (uint16_t)((pci_read16(d, d->cap_msix + 2) & 0x7FF) + 1);

    ksnprintf(d->name, sizeof(d->name), "0000:%02x:%02x.%x", bus, dev, func);
    d->dev.name = d->name;
    d->dev.dma_mask = ~0ULL;

    device_count++;
}

static void pci_scan_device(uint8_t bus, uint8_t dev) {
    uint32_t id_reg = pci_config_read32(bus, dev, 0, PCI_REG_VENDOR_ID);
    uint16_t vendor = id_reg & PCI_WORD0_MASK;
    uint16_t devid  = (id_reg >> PCI_WORD1_SHIFT) & PCI_WORD0_MASK;
    if (vendor == PCI_VENDOR_NONE || vendor == PCI_VENDOR_INVALID) return;

    pci_populate_device(bus, dev, 0, vendor, devid);

    /* Multi-function device? Read the header type from the device itself: once
     * the table is full, function 0 was not recorded and the last table entry is
     * some other device (M1-04 fix). */
    uint8_t header = pci_reg_byte(pci_config_read32(bus, dev, 0, PCI_REG_HEADER_TYPE), 2);
    if (header & PCI_HEADER_MULTIFUNCTION) {
        for (uint8_t func = 1; func < PCI_MAX_FUNCTIONS; func++) {
            id_reg = pci_config_read32(bus, dev, func, PCI_REG_VENDOR_ID);
            vendor = id_reg & PCI_WORD0_MASK;
            devid  = (id_reg >> PCI_WORD1_SHIFT) & PCI_WORD0_MASK;
            if (vendor == PCI_VENDOR_NONE || vendor == PCI_VENDOR_INVALID) continue;
            pci_populate_device(bus, dev, func, vendor, devid);
        }
    }
}

void pci_init(void) {
    device_count = 0;
    devices_dropped = 0;

    for (int bus = 0; bus < PCI_MAX_BUSES; bus++) {
        for (int dev = 0; dev < PCI_MAX_DEV_PER_BUS; dev++) {
            pci_scan_device((uint8_t)bus, (uint8_t)dev);
        }
    }

    kprintf("[PCI] Found %d devices\n", device_count);
    if (devices_dropped > 0) {
        kprintf("[PCI] WARNING: %d more not recorded (table holds %d)\n", devices_dropped, PCI_MAX_DEVICES);
    }
    for (int i = 0; i < device_count; i++) {
        PciDevice *d = &devices[i];
        kprintf("[PCI]   %x:%x.%x  %x:%x  class=%x:%x  IRQ=%d\n",
                d->addr.bus, d->addr.device, d->addr.function,
                d->vendor_id, d->device_id,
                d->class_code, d->subclass, d->irq_line);
    }
}

uint8_t pci_find_capability(const PciDevice *dev, uint8_t cap_id) {
    if (!(pci_read16(dev, PCI_REG_STATUS) & PCI_STATUS_CAP_LIST)) return 0;
    uint8_t ptr = (uint8_t)(pci_read16(dev, PCI_REG_CAP_PTR) & 0xFC);
    for (int n = 0; n < PCI_MAX_CAPS && ptr >= 0x40; n++) {
        uint16_t hdr = pci_read16(dev, ptr);
        if ((hdr & 0xFF) == cap_id) return ptr;
        ptr = (uint8_t)((hdr >> 8) & 0xFC);
    }
    return 0;
}

void pci_enable_bus_master(const PciDevice *dev) {
    uint16_t cmd = pci_read16(dev, PCI_REG_COMMAND);
    pci_write16(dev, PCI_REG_COMMAND, (uint16_t)(cmd | PCI_CMD_BUS_MASTER));
}

void pci_enable_decode(const PciDevice *dev, int mem) {
    uint16_t cmd = pci_read16(dev, PCI_REG_COMMAND);
    pci_write16(dev, PCI_REG_COMMAND, (uint16_t)(cmd | (mem ? PCI_CMD_MEM_SPACE : PCI_CMD_IO_SPACE)));
}

uint64_t pci_bar_phys(const PciDevice *dev, int i) {
    if (i < 0 || i >= PCI_NUM_BARS || dev->bar_size[i] == 0 || (dev->bar[i] & PCI_BAR_IO)) return 0;
    uint64_t base = dev->bar[i] & PCI_BAR_MEM_MASK;
    if ((dev->bar[i] & 0x6) == PCI_BAR_MEM_TYPE_64 && i + 1 < PCI_NUM_BARS) {
        base |= (uint64_t)dev->bar[i + 1] << 32;
    }
    return base;
}

volatile void *pci_bar_map(PciDevice *dev, int i) {
    uint64_t phys = pci_bar_phys(dev, i);
    if (phys == 0) return NULL;
    pci_enable_decode(dev, 1);
    return mmio_map(phys, dev->bar_size[i]);
}

/* ---- Matching and the PCI bus ---- */

/* 1 if want is PCI_ANY_ID or equals have. */
static int id_field_ok(uint16_t want, uint16_t have) {
    return want == PCI_ANY_ID || want == have;
}

const PciDeviceId *pci_match_id(const PciDeviceId *ids, const PciDevice *dev) {
    if (ids == NULL) return NULL;
    uint32_t cls = ((uint32_t)dev->class_code << 16) | ((uint32_t)dev->subclass << 8) | dev->prog_if;
    for (const PciDeviceId *id = ids; id->vendor != 0; id++) {
        if (id_field_ok(id->vendor, dev->vendor_id) && id_field_ok(id->device, dev->device_id) &&
            id_field_ok(id->subvendor, dev->subsys_vendor) && id_field_ok(id->subdevice, dev->subsys_id) &&
            ((cls ^ id->class_code) & id->class_mask) == 0) {
            return id;
        }
    }
    return NULL;
}

static int pci_bus_match(Device *dev, Driver *drv) {
    PciDriver *pdrv = container_of(drv, PciDriver, driver);
    return pci_match_id(pdrv->ids, to_pci_device(dev)) != NULL;
}

static int pci_bus_probe(Device *dev, Driver *drv) {
    PciDriver *pdrv = container_of(drv, PciDriver, driver);
    PciDevice *pdev = to_pci_device(dev);
    const PciDeviceId *id = pci_match_id(pdrv->ids, pdev);
    if (id == NULL || pdrv->probe == NULL) return -1;
    int rc = pdrv->probe(pdev, id);
    if (rc != 0) pci_free_irq_vectors(pdev);   /* a failed probe leaves no interrupts behind */
    return rc;
}

static void pci_bus_remove(Device *dev) {
    PciDevice *pdev = to_pci_device(dev);
    PciDriver *pdrv = container_of(dev->driver, PciDriver, driver);
    if (pdrv->remove != NULL) pdrv->remove(pdev);
    pci_free_irq_vectors(pdev);
}

static Bus pci_bus = {
    .name = "pci",
    .match = pci_bus_match,
    .probe = pci_bus_probe,
    .remove = pci_bus_remove,
};

void pci_bus_init(void) {
    bus_register(&pci_bus);
    for (int i = 0; i < device_count; i++) bus_add_device(&pci_bus, &devices[i].dev);
}

int pci_register_driver(PciDriver *drv) {
    return driver_register(&pci_bus, &drv->driver);
}

uint16_t pci_bar_io_base(uint32_t bar) {
    /* Bit 0 = 1 means I/O space. Bits [1:0] are flags, base is bits [31:2]. */
    return (uint16_t)(bar & PCI_BAR_IO_MASK);
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
    case 0x01:
        switch (subclass) {
        case 0x01:
            return "IDE";
        case 0x06:
            return "SATA";
        case 0x08:
            return "NVMe";
        default:
            return "storage";
        }
    case 0x02:
        return subclass == 0x00 ? "ethernet" : "network";
    case 0x03:
        return "display";
    case 0x04:
        return "audio/video";
    case 0x05:
        return "memory";
    case 0x06:
        switch (subclass) {
        case 0x00:
            return "host bridge";
        case 0x01:
            return "ISA bridge";
        case 0x04:
            return "PCI bridge";
        default:
            return "bridge";
        }
    case 0x07:
        return "serial/comm";
    case 0x08:
        return "system";
    case 0x09:
        return "input";
    case 0x0C:
        switch (subclass) {
        case 0x03:
            return "USB";
        case 0x05:
            return "SMBus";
        case 0x80:
            return "serial bus";
        default:
            return "bus";
        }
    case 0x0D:
        return "wireless";
    case 0x10:
        return "crypto";
    case 0x11:
        return "signal proc";
    case 0x12:
        return "accelerator";
    case 0x13:
        return "instrument";
    default:
        return "other";
    }
}

int pci_get_dropped_count(void) {
    return devices_dropped;
}

int pci_get_device_count(void) {
    return device_count;
}

PciDevice *pci_get_device(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}
