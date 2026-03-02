#include "drivers/pci.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"
#include <stddef.h>

static PciDevice devices[PCI_MAX_DEVICES];
static int device_count;

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31)             /* Enable bit */
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)(device & 0x1F) << 11)
                  | ((uint32_t)(func & 0x07) << 8)
                  | (offset & 0xFC);       /* Aligned to 4 bytes */
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t addr = (1U << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)(device & 0x1F) << 11)
                  | ((uint32_t)(func & 0x07) << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
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
    d->class_code = (class_reg >> 24) & 0xFF;
    d->subclass   = (class_reg >> 16) & 0xFF;
    d->prog_if    = (class_reg >>  8) & 0xFF;
    d->revision   = class_reg & 0xFF;

    /* Header type is byte 2 of the dword at offset 0x0C */
    d->header_type = (pci_config_read32(bus, dev, func, PCI_REG_HEADER_TYPE) >> 16) & 0xFF;

    /* Read BARs (only for header type 0 — normal devices) */
    if ((d->header_type & 0x7F) == 0) {
        for (int i = 0; i < 6; i++) {
            d->bar[i] = pci_config_read32(bus, dev, func, PCI_REG_BAR0 + i * 4);
        }
    }

    uint32_t irq_reg = pci_config_read32(bus, dev, func, PCI_REG_IRQ_LINE);
    d->irq_line = irq_reg & 0xFF;
    d->irq_pin  = (irq_reg >> 8) & 0xFF;

    device_count++;
}

static void pci_scan_device(uint8_t bus, uint8_t dev) {
    uint32_t id_reg = pci_config_read32(bus, dev, 0, PCI_REG_VENDOR_ID);
    uint16_t vendor = id_reg & 0xFFFF;
    uint16_t devid  = (id_reg >> 16) & 0xFFFF;
    if (vendor == PCI_VENDOR_NONE || vendor == 0x0000) return;

    pci_populate_device(bus, dev, 0, vendor, devid);

    /* Check if multi-function device — reuse header_type from the device
     * we just populated (avoids a redundant config read). */
    if (device_count > 0 && (devices[device_count - 1].header_type & PCI_HEADER_MULTIFUNCTION)) {
        for (uint8_t func = 1; func < 8; func++) {
            id_reg = pci_config_read32(bus, dev, func, PCI_REG_VENDOR_ID);
            vendor = id_reg & 0xFFFF;
            devid  = (id_reg >> 16) & 0xFFFF;
            if (vendor == PCI_VENDOR_NONE || vendor == 0x0000) continue;
            pci_populate_device(bus, dev, func, vendor, devid);
        }
    }
}

void pci_init(void) {
    device_count = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
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
    return (uint16_t)(bar & 0xFFFC);
}

int pci_get_device_count(void) {
    return device_count;
}

const PciDevice *pci_get_device(int index) {
    if (index < 0 || index >= device_count) return (void *)0;
    return &devices[index];
}
