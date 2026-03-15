#include "drivers/pci.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"
#include <stddef.h>

/* PCI config address construction */
#define PCI_ENABLE_BIT          (1U << 31)
#define PCI_DEVICE_MASK         0x1F
#define PCI_FUNCTION_MASK       0x07
#define PCI_OFFSET_MASK         0xFC

/* Header and BAR parsing */
#define PCI_HEADER_TYPE_MASK    0x7F
#define PCI_NUM_BARS            6
#define PCI_BAR_STRIDE          4
#define PCI_BAR_IO_MASK         0xFFFC

/* Word extraction from 32-bit PCI config registers */
#define PCI_WORD0_MASK   0xFFFF
#define PCI_WORD1_SHIFT  16

/* Bus scan limits */
#define PCI_MAX_BUSES           256
#define PCI_MAX_DEV_PER_BUS     32
#define PCI_MAX_FUNCTIONS       8

/* Vendor ID indicating absent device */
#define PCI_VENDOR_INVALID      0x0000

static PciDevice devices[PCI_MAX_DEVICES];
static int device_count;

static inline uint32_t pci_config_addr(uint8_t bus, uint8_t device,
                                        uint8_t func, uint8_t offset) {
    return PCI_ENABLE_BIT
         | ((uint32_t)bus << 16)
         | ((uint32_t)(device & PCI_DEVICE_MASK) << 11)
         | ((uint32_t)(func & PCI_FUNCTION_MASK) << 8)
         | (offset & PCI_OFFSET_MASK);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, device, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, device, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

/* Extract a single byte (0-3) from a 32-bit PCI config register. */
static inline uint8_t pci_reg_byte(uint32_t reg, int byte) {
    return (uint8_t)((reg >> (byte * 8)) & 0xFF);
}

/* Populate a PciDevice entry. Caller already verified vendor != 0xFFFF. */
static void pci_populate_device(uint8_t bus, uint8_t dev, uint8_t func,
                                uint16_t vendor, uint16_t devid) {
    if (device_count >= PCI_MAX_DEVICES) return;

    PciDevice *d = &devices[device_count];
    d->addr.bus      = bus;
    d->addr.device   = dev;
    d->addr.function = func;
    d->vendor_id     = vendor;
    d->device_id     = devid;

    uint32_t class_reg = pci_config_read32(bus, dev, func, PCI_REG_CLASS);
    d->class_code = pci_reg_byte(class_reg, 3);
    d->subclass   = pci_reg_byte(class_reg, 2);
    d->prog_if    = pci_reg_byte(class_reg, 1);
    d->revision   = pci_reg_byte(class_reg, 0);

    /* Header type is byte 2 of the dword at offset 0x0C */
    d->header_type = pci_reg_byte(pci_config_read32(bus, dev, func, PCI_REG_HEADER_TYPE), 2);

    /* Read BARs (only for header type 0 — normal devices) */
    if ((d->header_type & PCI_HEADER_TYPE_MASK) == 0) {
        for (int i = 0; i < PCI_NUM_BARS; i++) {
            d->bar[i] = pci_config_read32(bus, dev, func, PCI_REG_BAR0 + i * PCI_BAR_STRIDE);
        }
    }

    uint32_t irq_reg = pci_config_read32(bus, dev, func, PCI_REG_IRQ_LINE);
    d->irq_line = pci_reg_byte(irq_reg, 0);
    d->irq_pin  = pci_reg_byte(irq_reg, 1);

    device_count++;
}

static void pci_scan_device(uint8_t bus, uint8_t dev) {
    uint32_t id_reg = pci_config_read32(bus, dev, 0, PCI_REG_VENDOR_ID);
    uint16_t vendor = id_reg & PCI_WORD0_MASK;
    uint16_t devid  = (id_reg >> PCI_WORD1_SHIFT) & PCI_WORD0_MASK;
    if (vendor == PCI_VENDOR_NONE || vendor == PCI_VENDOR_INVALID) return;

    pci_populate_device(bus, dev, 0, vendor, devid);

    /* Check if multi-function device — reuse header_type from the device
     * we just populated (avoids a redundant config read). */
    if (device_count > 0 && (devices[device_count - 1].header_type & PCI_HEADER_MULTIFUNCTION)) {
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

    for (int bus = 0; bus < PCI_MAX_BUSES; bus++) {
        for (int dev = 0; dev < PCI_MAX_DEV_PER_BUS; dev++) {
            pci_scan_device((uint8_t)bus, (uint8_t)dev);
        }
    }

    kprintf("[PCI] Found %d devices\n", device_count);
    for (int i = 0; i < device_count; i++) {
        PciDevice *d = &devices[i];
        kprintf("[PCI]   %x:%x.%x  %x:%x  class=%x:%x  IRQ=%d\n",
                d->addr.bus, d->addr.device, d->addr.function,
                d->vendor_id, d->device_id,
                d->class_code, d->subclass, d->irq_line);
    }
}

const PciDevice *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id) {
            return &devices[i];
        }
    }
    return NULL;
}

void pci_enable_bus_master(const PciDevice *dev) {
    uint32_t cmd = pci_config_read32(dev->addr.bus, dev->addr.device,
                                     dev->addr.function, PCI_REG_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write32(dev->addr.bus, dev->addr.device,
                       dev->addr.function, PCI_REG_COMMAND, cmd);
}

uint16_t pci_bar_io_base(uint32_t bar) {
    /* Bit 0 = 1 means I/O space. Bits [1:0] are flags, base is bits [31:2]. */
    return (uint16_t)(bar & PCI_BAR_IO_MASK);
}

int pci_get_device_count(void) {
    return device_count;
}

const PciDevice *pci_get_device(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}
