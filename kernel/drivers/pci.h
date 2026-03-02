#ifndef ARCHOS_DRIVERS_PCI_H
#define ARCHOS_DRIVERS_PCI_H

#include <stdint.h>

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
#define PCI_REG_IRQ_LINE      0x3C
#define PCI_REG_IRQ_PIN       0x3D

/* PCI command register bits */
#define PCI_CMD_IO_SPACE      (1 << 0)
#define PCI_CMD_MEM_SPACE     (1 << 1)
#define PCI_CMD_BUS_MASTER    (1 << 2)

/* Header type bit 7 = multi-function */
#define PCI_HEADER_MULTIFUNCTION  0x80

/* Invalid vendor ID (slot empty) */
#define PCI_VENDOR_NONE  0xFFFF

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} PciAddress;

typedef struct {
    PciAddress addr;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint8_t irq_pin;
    uint32_t bar[6];
} PciDevice;

/* Read a 32-bit value from PCI config space. offset must be 4-byte aligned. */
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);

/* Write a 32-bit value to PCI config space. offset must be 4-byte aligned. */
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);

/* Scan all PCI buses and populate the device table. */
void pci_init(void);

/* Find a device by vendor and device ID. Returns NULL if not found. */
const PciDevice *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Enable bus mastering (DMA) for a device. */
void pci_enable_bus_master(const PciDevice *dev);

/* Extract I/O port base address from a BAR value (clears flag bits). */
uint16_t pci_bar_io_base(uint32_t bar);

/* Get the number of discovered devices. */
int pci_get_device_count(void);

/* Get a device by index (0..count-1). */
const PciDevice *pci_get_device(int index);

#endif /* ARCHOS_DRIVERS_PCI_H */
