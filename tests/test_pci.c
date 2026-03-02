/* arc_os — Host-side tests for kernel/drivers/pci.c enumeration logic */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_ARCH_X86_64_IO_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* --- Fake PCI config space --- */

/* We simulate a 256-bus × 32-device × 8-function config space.
 * Only a small portion is populated; the rest returns 0xFFFFFFFF (no device). */

#define FAKE_MAX_SLOTS 8

typedef struct {
    uint8_t bus, device, function;
    uint32_t config[64];  /* 256 bytes of config space as 32-bit words */
} FakeSlot;

static FakeSlot fake_slots[FAKE_MAX_SLOTS];
static int fake_slot_count;

static void fake_reset(void) {
    fake_slot_count = 0;
    memset(fake_slots, 0, sizeof(fake_slots));
}

static FakeSlot *fake_find(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < fake_slot_count; i++) {
        if (fake_slots[i].bus == bus &&
            fake_slots[i].device == dev &&
            fake_slots[i].function == func) {
            return &fake_slots[i];
        }
    }
    return NULL;
}

static FakeSlot *fake_add(uint8_t bus, uint8_t dev, uint8_t func,
                          uint16_t vendor, uint16_t devid,
                          uint8_t class, uint8_t subclass) {
    if (fake_slot_count >= FAKE_MAX_SLOTS) return NULL;
    FakeSlot *s = &fake_slots[fake_slot_count++];
    s->bus = bus;
    s->device = dev;
    s->function = func;
    memset(s->config, 0xFF, sizeof(s->config));
    /* Vendor/Device ID at offset 0 */
    s->config[0] = (uint32_t)vendor | ((uint32_t)devid << 16);
    /* Class/subclass/prog-if/rev at offset 0x08 */
    s->config[2] = ((uint32_t)class << 24) | ((uint32_t)subclass << 16);
    /* Header type at offset 0x0C (byte 2 of dword 3) — default 0 (single-function) */
    s->config[3] = 0;
    /* IRQ line/pin at offset 0x3C */
    s->config[15] = 0;  /* IRQ 0, no pin */
    return s;
}

/* Stub I/O port operations */
static uint32_t last_config_addr;

static void outl(uint16_t port, uint32_t value) {
    if (port == 0x0CF8) {
        last_config_addr = value;
    }
    /* Writes to 0x0CFC handled in pci_config_write32 */
}

static uint32_t inl(uint16_t port) {
    if (port == 0x0CFC) {
        /* Decode the config address */
        uint8_t bus  = (last_config_addr >> 16) & 0xFF;
        uint8_t dev  = (last_config_addr >> 11) & 0x1F;
        uint8_t func = (last_config_addr >> 8) & 0x07;
        uint8_t off  = (last_config_addr) & 0xFC;

        FakeSlot *s = fake_find(bus, dev, func);
        if (s) {
            return s->config[off / 4];
        }
        return 0xFFFFFFFF;  /* No device */
    }
    return 0xFFFFFFFF;
}

/* Stubs for 16-bit I/O (not used by PCI but declared in io.h) */
static void outw(uint16_t port, uint16_t value) { (void)port; (void)value; }
static void outb(uint16_t port, uint8_t value) { (void)port; (void)value; }
static uint16_t inw(uint16_t port) { (void)port; return 0xFFFF; }
static uint8_t inb(uint16_t port) { (void)port; return 0xFF; }

/* Include the real PCI implementation */
#include "../kernel/drivers/pci.c"

/* --- Tests --- */

static int test_pci_empty_bus(void) {
    fake_reset();
    pci_init();
    ASSERT_EQ(pci_get_device_count(), 0);
    return 0;
}

static int test_pci_single_device(void) {
    fake_reset();
    /* VirtIO block device on bus 0, device 1, function 0 */
    fake_add(0, 1, 0, 0x1AF4, 0x1001, 0x01, 0x80);
    pci_init();
    ASSERT_EQ(pci_get_device_count(), 1);

    const PciDevice *d = pci_get_device(0);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(d->vendor_id, 0x1AF4);
    ASSERT_EQ(d->device_id, 0x1001);
    ASSERT_EQ(d->class_code, 0x01);
    ASSERT_EQ(d->subclass, 0x80);
    ASSERT_EQ(d->addr.bus, 0);
    ASSERT_EQ(d->addr.device, 1);
    ASSERT_EQ(d->addr.function, 0);
    return 0;
}

static int test_pci_find_device(void) {
    fake_reset();
    fake_add(0, 1, 0, 0x1AF4, 0x1001, 0x01, 0x80);
    fake_add(0, 2, 0, 0x8086, 0x1234, 0x03, 0x00);
    pci_init();

    const PciDevice *virtio = pci_find_device(0x1AF4, 0x1001);
    ASSERT_TRUE(virtio != NULL);
    ASSERT_EQ(virtio->vendor_id, 0x1AF4);

    const PciDevice *intel = pci_find_device(0x8086, 0x1234);
    ASSERT_TRUE(intel != NULL);
    ASSERT_EQ(intel->vendor_id, 0x8086);

    const PciDevice *missing = pci_find_device(0xDEAD, 0xBEEF);
    ASSERT_TRUE(missing == NULL);

    return 0;
}

static int test_pci_multifunction(void) {
    fake_reset();
    /* Multi-function device: set bit 7 of header type */
    FakeSlot *s = fake_add(0, 3, 0, 0x1AF4, 0x1000, 0x02, 0x00);
    s->config[3] = (0x80U << 16);  /* header_type = 0x80 (multifunction) */
    /* Add function 1 */
    fake_add(0, 3, 1, 0x1AF4, 0x1001, 0x01, 0x80);
    pci_init();

    ASSERT_EQ(pci_get_device_count(), 2);

    const PciDevice *d0 = pci_get_device(0);
    ASSERT_EQ(d0->addr.function, 0);
    const PciDevice *d1 = pci_get_device(1);
    ASSERT_EQ(d1->addr.function, 1);
    return 0;
}

static int test_pci_bar_io_base(void) {
    /* I/O BAR: bit 0 = 1, base in bits [31:2] */
    ASSERT_EQ(pci_bar_io_base(0xC041), 0xC040);
    ASSERT_EQ(pci_bar_io_base(0x0001), 0x0000);
    ASSERT_EQ(pci_bar_io_base(0x1F01), 0x1F00);
    return 0;
}

static int test_pci_get_device_bounds(void) {
    fake_reset();
    pci_init();
    ASSERT_TRUE(pci_get_device(-1) == NULL);
    ASSERT_TRUE(pci_get_device(0) == NULL);
    ASSERT_TRUE(pci_get_device(100) == NULL);
    return 0;
}

/* --- Test suite export --- */

TestCase pci_tests[] = {
    { "pci_empty_bus",         test_pci_empty_bus },
    { "pci_single_device",    test_pci_single_device },
    { "pci_find_device",      test_pci_find_device },
    { "pci_multifunction",    test_pci_multifunction },
    { "pci_bar_io_base",      test_pci_bar_io_base },
    { "pci_get_device_bounds", test_pci_get_device_bounds },
};

int pci_test_count = sizeof(pci_tests) / sizeof(pci_tests[0]);
