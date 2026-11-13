/* arc_os — Host-side tests for kernel/drivers/pci.c and pci_irq.c
 *
 * A fake config space answers the type-1 ports (32- and 16-bit), keeps
 * writes, and models BAR sizing (a BAR keeps only the address bits its size
 * allows). MSI-X tables live in fake BAR memory that mmio_map hands back. The
 * IRQ layer is faked (renamed, so it doesn't meet test_irq.c's real irq.c);
 * the bus is the real bus.c from test_bus.c. */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_ARCH_X86_64_IO_H
#define ARCHOS_MM_KMALLOC_H

#include <stddef.h>
#include "mm/gfp.h"

static inline void kprintf(const char *fmt, ...) { (void)fmt; }
int ksnprintf(char *buf, size_t size, const char *fmt, ...);   /* the real one (test_kprintf.c) */

static void *kmalloc(size_t size, uint32_t flags) {
    return (flags & GFP_ZERO) ? calloc(1, size) : malloc(size);
}

static void kfree(void *p) {
    free(p);
}

/* --- Fake PCI config space --- */

#define FAKE_MAX_SLOTS 80

typedef struct {
    uint8_t bus, device, function;
    uint32_t config[64];   /* 256 bytes of config space as 32-bit words */
    uint32_t bar_mask[6];  /* address bits each BAR keeps (0 = not implemented) */
} FakeSlot;

static FakeSlot fake_slots[FAKE_MAX_SLOTS];
static int fake_slot_count;

static void fake_reset(void) {
    fake_slot_count = 0;
    memset(fake_slots, 0, sizeof(fake_slots));
}

static FakeSlot *fake_find(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < fake_slot_count; i++) {
        if (fake_slots[i].bus == bus && fake_slots[i].device == dev && fake_slots[i].function == func) {
            return &fake_slots[i];
        }
    }
    return NULL;
}

static FakeSlot *fake_add(uint8_t bus, uint8_t dev, uint8_t func, uint16_t vendor, uint16_t devid, uint8_t class,
                          uint8_t subclass) {
    if (fake_slot_count >= FAKE_MAX_SLOTS) return NULL;
    FakeSlot *s = &fake_slots[fake_slot_count++];
    s->bus = bus;
    s->device = dev;
    s->function = func;
    s->config[0] = (uint32_t)vendor | ((uint32_t)devid << 16);
    s->config[2] = ((uint32_t) class << 24) | ((uint32_t)subclass << 16);
    return s;
}

/* Memory BAR i at base, size bytes (power of two); 64-bit takes BARs i, i+1 */
static void fake_mem_bar(FakeSlot *s, int i, uint64_t base, uint64_t size, int is64) {
    uint64_t mask = ~(size - 1);
    s->config[4 + i] = (uint32_t)base | (is64 ? 0x4u : 0u);
    s->bar_mask[i] = (uint32_t)mask & 0xFFFFFFF0u;
    if (is64) {
        s->config[5 + i] = (uint32_t)(base >> 32);
        s->bar_mask[i + 1] = (uint32_t)(mask >> 32);
    }
}

static void fake_io_bar(FakeSlot *s, int i, uint16_t base, uint32_t size) {
    s->config[4 + i] = (uint32_t)base | 0x1u;
    s->bar_mask[i] = ~(size - 1) & 0xFFFFFFFCu;
}

/* A capability with id at off, linked to next (0 ends the list) */
static void fake_cap(FakeSlot *s, uint8_t off, uint8_t id, uint8_t next, uint16_t ctrl) {
    s->config[1] |= (uint32_t)0x10 << 16;   /* status: capability list */
    if ((s->config[13] & 0xFF) == 0) s->config[13] = off;
    s->config[off / 4] = id | ((uint32_t)next << 8) | ((uint32_t)ctrl << 16);
}

static uint32_t last_config_addr;
static int command_writes;

static FakeSlot *addressed(uint8_t *off) {
    *off = last_config_addr & 0xFC;
    return fake_find((last_config_addr >> 16) & 0xFF, (last_config_addr >> 11) & 0x1F, (last_config_addr >> 8) & 0x07);
}

static void store(FakeSlot *s, uint8_t off, uint32_t value) {
    if (off >= 0x10 && off < 0x28) {
        int i = (off - 0x10) / 4;
        uint32_t flags = s->config[off / 4] & ((s->config[off / 4] & 1) ? 0x3u : 0xFu);
        if (i > 0 && (s->config[3 + i] & 0x7) == 0x4) flags = 0;   /* upper half of a 64-bit BAR */
        s->config[off / 4] = (value & s->bar_mask[i]) | flags;
        return;
    }
    if (off == 0x04) command_writes++;
    s->config[off / 4] = value;
}

static void outl(uint16_t port, uint32_t value) {
    if (port == 0x0CF8) {
        last_config_addr = value;
        return;
    }
    uint8_t off;
    FakeSlot *s = addressed(&off);
    if (port == 0x0CFC && s != NULL) store(s, off, value);
}

static uint32_t inl(uint16_t port) {
    uint8_t off;
    FakeSlot *s = addressed(&off);
    if (port != 0x0CFC || s == NULL) return 0xFFFFFFFF;
    return s->config[off / 4];
}

static void outw(uint16_t port, uint16_t value) {
    uint8_t off;
    FakeSlot *s = addressed(&off);
    if (s == NULL || (port & ~2) != 0x0CFC) return;
    int shift = (port & 2) * 8;
    uint32_t v = (s->config[off / 4] & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    store(s, off, v);
}

static uint16_t inw(uint16_t port) {
    uint8_t off;
    FakeSlot *s = addressed(&off);
    if (s == NULL || (port & ~2) != 0x0CFC) return 0xFFFF;
    return (uint16_t)(s->config[off / 4] >> ((port & 2) * 8));
}


/* --- Fake device memory: mmio_map returns it for addresses in FAKE_MMIO_BASE --- */

#define FAKE_MMIO_BASE 0xFE000000ULL
#define FAKE_MMIO_SIZE 0x4000
static uint8_t fake_mmio[FAKE_MMIO_SIZE] __attribute__((aligned(4096)));
static int mmio_maps;

volatile void *mmio_map(uint64_t phys, uint64_t size) {
    mmio_maps++;
    if (phys < FAKE_MMIO_BASE || phys + size > FAKE_MMIO_BASE + FAKE_MMIO_SIZE) return NULL;
    return fake_mmio + (phys - FAKE_MMIO_BASE);
}

/* --- Fake IRQ layer (renamed past the real irq.c in test_irq.c) --- */

#define irq_desc_create   pci_test_irq_desc_create
#define irq_desc_destroy  pci_test_irq_desc_destroy
#define irq_free          pci_test_irq_free
#define irq_msi_available pci_test_irq_msi_available

#include "drivers/irq.h"

static int msi_available = 1;
static int descs_live;
static int frees;

int pci_test_irq_msi_available(void) {
    return msi_available;
}

IrqDesc *pci_test_irq_desc_create(Device *dev, uint8_t kind, const IrqChip *chip, void *chip_data, uint16_t hwindex,
                                  uint8_t line) {
    IrqDesc *d = calloc(1, sizeof(*d));
    d->dev = dev;
    d->kind = kind;
    d->chip = chip;
    d->chip_data = chip_data;
    d->hwindex = hwindex;
    d->line = line;
    descs_live++;
    return d;
}

void pci_test_irq_desc_destroy(IrqDesc *d) {
    descs_live--;
    free(d);
}

void pci_test_irq_free(Device *dev, unsigned index) {
    (void)dev;
    (void)index;
    frees++;
}

/* Include the real PCI implementation */
#include "../kernel/drivers/pci.c"
#include "../kernel/drivers/pci_irq.c"

static uint32_t cfg(int slot, uint8_t off) {
    return fake_slots[slot].config[off / 4];
}

static uint16_t cfg16(int slot, uint8_t off) {
    return (uint16_t)(cfg(slot, off & ~3) >> ((off & 2) * 8));
}

static void irq_reset(void) {
    msi_available = 1;
    descs_live = 0;
    frees = 0;
    mmio_maps = 0;
    memset(fake_mmio, 0, sizeof(fake_mmio));
}

/* --- Enumeration --- */

static int test_pci_empty_bus(void) {
    fake_reset();
    pci_init();
    ASSERT_EQ(pci_get_device_count(), 0);
    return 0;
}

static int test_pci_single_device(void) {
    fake_reset();
    FakeSlot *s = fake_add(0, 1, 0, 0x1AF4, 0x1001, 0x01, 0x80);
    s->config[11] = 0x00021AF4;   /* subsystem vendor 1AF4, subsystem 2 */
    s->config[15] = 0x0000010B;   /* pin A, line 11 */
    pci_init();
    ASSERT_EQ(pci_get_device_count(), 1);

    const PciDevice *d = pci_get_device(0);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(d->vendor_id, 0x1AF4);
    ASSERT_EQ(d->device_id, 0x1001);
    ASSERT_EQ(d->subsys_vendor, 0x1AF4);
    ASSERT_EQ(d->subsys_id, 2);
    ASSERT_EQ(d->class_code, 0x01);
    ASSERT_EQ(d->subclass, 0x80);
    ASSERT_EQ(d->irq_line, 11);
    ASSERT_EQ(d->irq_pin, 1);
    ASSERT_EQ(d->addr.bus, 0);
    ASSERT_EQ(d->addr.device, 1);
    ASSERT_EQ(d->addr.function, 0);
    ASSERT_STR_EQ(d->name, "0000:00:01.0");
    ASSERT_TRUE(d->dev.name == d->name);
    ASSERT_TRUE(d->dev.dma_mask == ~0ULL);
    ASSERT_TRUE(pci_driver_name(d) == NULL);
    return 0;
}

static int test_pci_multifunction(void) {
    fake_reset();
    FakeSlot *s = fake_add(0, 3, 0, 0x1AF4, 0x1000, 0x02, 0x00);
    s->config[3] = (0x80U << 16);   /* header_type = 0x80 (multifunction) */
    fake_add(0, 3, 1, 0x1AF4, 0x1001, 0x01, 0x80);
    pci_init();

    ASSERT_EQ(pci_get_device_count(), 2);
    ASSERT_EQ(pci_get_device(0)->addr.function, 0);
    ASSERT_EQ(pci_get_device(1)->addr.function, 1);
    ASSERT_STR_EQ(pci_get_device(1)->name, "0000:00:03.1");
    return 0;
}

/* M1-04: with the table full, a multi-function device's functions are still
 * found (and counted as dropped): the header type is read from the device, not
 * from the last table entry */
static int test_pci_multifunction_after_table_full(void) {
    fake_reset();
    for (int i = 0; i < PCI_MAX_DEVICES; i++) {
        fake_add((uint8_t)(i / 32), (uint8_t)(i % 32), 0, 0x8086, (uint16_t)i, 0x02, 0x00);
    }
    FakeSlot *mf = fake_add(5, 0, 0, 0x8086, 0x1111, 0x0C, 0x03);
    mf->config[3] = (0x80U << 16);
    fake_add(5, 0, 1, 0x8086, 0x2222, 0x0C, 0x03);
    pci_init();
    ASSERT_EQ(pci_get_device_count(), PCI_MAX_DEVICES);
    ASSERT_EQ(pci_get_dropped_count(), 2);
    return 0;
}

static int test_pci_class_names(void) {
    ASSERT_STR_EQ(pci_class_name(0x01, 0x08), "NVMe");
    ASSERT_STR_EQ(pci_class_name(0x01, 0x06), "SATA");
    ASSERT_STR_EQ(pci_class_name(0x02, 0x00), "ethernet");
    ASSERT_STR_EQ(pci_class_name(0x0C, 0x03), "USB");
    ASSERT_STR_EQ(pci_class_name(0xEE, 0x00), "other");
    return 0;
}

static int test_pci_bar_io_base(void) {
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

/* --- BARs --- */

/* Sizes come from the all-ones probe; values and the command register are
 * restored; 64-bit BARs are joined and their upper half has no size */
static int test_pci_bar_sizing(void) {
    fake_reset();
    FakeSlot *s = fake_add(0, 4, 0, 0x8086, 0x10D3, 0x02, 0x00);
    fake_mem_bar(s, 0, 0xFEB00000, 0x20000, 0);        /* 128 KiB, 32-bit */
    fake_io_bar(s, 2, 0xC000, 32);                     /* 32 ports */
    fake_mem_bar(s, 3, 0x800000004000ULL, 0x4000, 1);  /* 16 KiB, 64-bit, above 4 GiB */
    s->config[1] = PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE;
    pci_init();
    const PciDevice *d = pci_get_device(0);
    ASSERT_EQ(d->bar_size[0], 0x20000);
    ASSERT_EQ(d->bar_size[1], 0);
    ASSERT_EQ(d->bar_size[2], 32);
    ASSERT_EQ(d->bar_size[3], 0x4000);
    ASSERT_EQ(d->bar_size[4], 0);
    ASSERT_EQ(d->bar_size[5], 0);
    ASSERT_EQ(cfg(0, 0x10), 0xFEB00000);   /* restored */
    ASSERT_EQ(cfg(0, 0x18), 0xC001);
    ASSERT_EQ(cfg(0, 0x1C), 0x4004);
    ASSERT_EQ(cfg(0, 0x20), 0x8000);
    ASSERT_EQ(cfg16(0, 0x04), PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE);
    ASSERT_EQ(pci_bar_phys(d, 0), 0xFEB00000);
    ASSERT_EQ(pci_bar_phys(d, 2), 0);   /* I/O */
    ASSERT_TRUE(pci_bar_phys(d, 3) == 0x800000004000ULL);
    ASSERT_EQ(pci_bar_phys(d, 4), 0);
    ASSERT_EQ(pci_bar_phys(d, 6), 0);
    return 0;
}

/* Decoding is off while BARs are probed (so the probe address never decodes) */
static int test_pci_bar_sizing_decode_off(void) {
    fake_reset();
    FakeSlot *s = fake_add(0, 4, 0, 0x8086, 0x10D3, 0x02, 0x00);
    fake_mem_bar(s, 0, 0xFEB00000, 0x1000, 0);
    s->config[1] = 0x00100000 | PCI_CMD_MEM_SPACE;   /* status bit set: a 16-bit write must not touch it */
    command_writes = 0;
    pci_init();
    ASSERT_EQ(command_writes, 2);                   /* off, then restored */
    ASSERT_EQ(cfg(0, 0x04), 0x00100000 | PCI_CMD_MEM_SPACE);
    return 0;
}

static int test_pci_enable_bits(void) {
    fake_reset();
    fake_add(0, 4, 0, 0x8086, 0x10D3, 0x02, 0x00);
    pci_init();
    const PciDevice *d = pci_get_device(0);
    pci_enable_bus_master(d);
    pci_enable_decode(d, 0);
    ASSERT_EQ(cfg16(0, 0x04), PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE);
    pci_enable_decode(d, 1);
    ASSERT_EQ(cfg16(0, 0x04), PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE);
    return 0;
}

/* --- Capabilities --- */

static int test_pci_capabilities(void) {
    fake_reset();
    FakeSlot *s = fake_add(0, 4, 0, 0x8086, 0x10D3, 0x02, 0x00);
    fake_cap(s, 0x40, 0x01, 0x50, 0);        /* power management */
    fake_cap(s, 0x50, PCI_CAP_ID_MSI, 0x70, 0);
    fake_cap(s, 0x70, PCI_CAP_ID_MSIX, 0x00, 0x0002);   /* 3 entries */
    pci_init();
    const PciDevice *d = pci_get_device(0);
    ASSERT_EQ(d->cap_msi, 0x50);
    ASSERT_EQ(d->cap_msix, 0x70);
    ASSERT_EQ(d->msix_entries, 3);
    ASSERT_EQ(pci_find_capability(d, 0x01), 0x40);
    ASSERT_EQ(pci_find_capability(d, 0x10), 0);   /* PCIe: absent */
    return 0;
}

/* A looping or absent list ends: bounded walk, status bit honoured */
static int test_pci_capability_loop_and_absent(void) {
    fake_reset();
    FakeSlot *s = fake_add(0, 4, 0, 0x8086, 0x10D3, 0x02, 0x00);
    fake_cap(s, 0x40, 0x01, 0x40, 0);   /* points to itself */
    FakeSlot *t = fake_add(0, 5, 0, 0x8086, 0x10D3, 0x02, 0x00);
    t->config[13] = 0x40;               /* pointer set but status says no list */
    t->config[0x40 / 4] = PCI_CAP_ID_MSI;
    pci_init();
    ASSERT_EQ(pci_find_capability(pci_get_device(0), PCI_CAP_ID_MSI), 0);
    ASSERT_EQ(pci_find_capability(pci_get_device(1), PCI_CAP_ID_MSI), 0);
    return 0;
}

/* --- ID matching --- */

static int test_pci_match_id(void) {
    PciDevice d;
    memset(&d, 0, sizeof(d));
    d.vendor_id = 0x8086;
    d.device_id = 0x10D3;
    d.subsys_vendor = 0x17AA;
    d.subsys_id = 0x2233;
    d.class_code = 0x01;
    d.subclass = 0x08;
    d.prog_if = 0x02;

    const PciDeviceId none[] = {PCI_DEVICE(0x1AF4, 0x1001), {0}};
    ASSERT_TRUE(pci_match_id(none, &d) == NULL);
    ASSERT_TRUE(pci_match_id(NULL, &d) == NULL);

    const PciDeviceId exact[] = {PCI_DEVICE(0x1AF4, 0x1001), PCI_DEVICE(0x8086, 0x10D3), {0}};
    ASSERT_TRUE(pci_match_id(exact, &d) == &exact[1]);

    PciDeviceId sub[] = {PCI_DEVICE(0x8086, 0x10D3), PCI_DEVICE(0x8086, 0x10D3), {0}};
    sub[0].subvendor = 0x1028;   /* another maker's board: no */
    sub[1].subvendor = 0x17AA;
    sub[1].subdevice = 0x2233;
    ASSERT_TRUE(pci_match_id(sub, &d) == &sub[1]);

    const PciDeviceId nvme[] = {PCI_DEVICE_CLASS(0x010802, 0xFFFFFF), {0}};
    ASSERT_TRUE(pci_match_id(nvme, &d) == &nvme[0]);
    const PciDeviceId ahci[] = {PCI_DEVICE_CLASS(0x010601, 0xFFFFFF), {0}};
    ASSERT_TRUE(pci_match_id(ahci, &d) == NULL);
    const PciDeviceId storage[] = {PCI_DEVICE_CLASS(0x010000, 0xFF0000), {0}};
    ASSERT_TRUE(pci_match_id(storage, &d) == &storage[0]);
    return 0;
}

/* --- Binding through the real bus --- */

static int probes;
static int probe_rc;

static int edu_like_probe(PciDevice *pdev, const PciDeviceId *id) {
    (void)id;
    probes++;
    pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_INTX);
    return probe_rc;
}

static int test_pci_driver_binding(void) {
    fake_reset();
    irq_reset();
    FakeSlot *s = fake_add(0, 2, 0, 0x1234, 0x11E8, 0x00, 0xFF);
    s->config[15] = 0x0000010A;   /* pin A, line 10 */
    fake_add(0, 3, 0, 0x8086, 0x10D3, 0x02, 0x00);
    pci_init();
    pci_bus_init();

    static const PciDeviceId ids[] = {PCI_DEVICE(0x1234, 0x11E8), {0}};
    static PciDriver bad = {.driver = {.name = "bad"}, .ids = ids, .probe = edu_like_probe};
    static PciDriver good = {.driver = {.name = "good"}, .ids = ids, .probe = edu_like_probe};

    probes = 0;
    probe_rc = -5;
    ASSERT_EQ(pci_register_driver(&bad), 0);
    ASSERT_EQ(probes, 1);
    PciDevice *d = pci_get_device(0);
    ASSERT_TRUE(d->dev.irqs == NULL);   /* the failed probe's interrupts were freed */
    ASSERT_EQ(descs_live, 0);
    ASSERT_EQ(d->irq_kind, 0);

    probe_rc = 0;
    ASSERT_EQ(pci_register_driver(&good), 1);
    ASSERT_STR_EQ(pci_driver_name(d), "good");
    ASSERT_TRUE(pci_driver_name(pci_get_device(1)) == NULL);
    ASSERT_EQ(descs_live, 1);

    device_unbind(&d->dev);   /* remove frees the interrupts */
    ASSERT_TRUE(d->dev.irqs == NULL);
    ASSERT_EQ(descs_live, 0);
    return 0;
}

/* --- Interrupt allocation (pci_irq.c) --- */

/* Device with MSI (64-bit, per-vector mask) and a 4-entry MSI-X table in BAR1 */
static PciDevice *irq_device(int with_msix, int with_msi, uint16_t msi_ctrl) {
    fake_reset();
    irq_reset();
    FakeSlot *s = fake_add(0, 4, 0, 0x1AF4, 0x1001, 0x01, 0x00);
    fake_io_bar(s, 0, 0xC000, 64);
    fake_mem_bar(s, 1, FAKE_MMIO_BASE, 0x1000, 0);
    s->config[15] = 0x0000010B;   /* pin A, line 11 */
    uint8_t next = 0;
    if (with_msix) {
        fake_cap(s, 0x60, PCI_CAP_ID_MSIX, 0, 0x0003);   /* 4 entries */
        s->config[0x64 / 4] = 0x800 | 1;                /* table at BAR1 + 0x800 */
        next = 0x60;
    }
    if (with_msi) {
        s->config[13] = 0;
        fake_cap(s, 0x40, PCI_CAP_ID_MSI, next, msi_ctrl);
    } else if (with_msix) {
        s->config[13] = 0x60;
    }
    pci_init();
    return pci_get_device(0);
}

static volatile uint32_t *table_entry(int i) {
    return (volatile uint32_t *)(fake_mmio + 0x800 + i * 16);
}

static int test_pci_msix_alloc(void) {
    PciDevice *d = irq_device(1, 1, 0);
    ASSERT_EQ(d->msix_entries, 4);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 2, PCI_IRQ_ALL), 2);
    ASSERT_EQ(d->irq_kind, IRQ_KIND_MSIX);
    ASSERT_EQ(d->dev.nr_irqs, 2);
    ASSERT_EQ(descs_live, 2);
    ASSERT_STR_EQ(d->dev.irqs[1]->chip->name, "PCI-MSIX");
    ASSERT_EQ(d->dev.irqs[1]->hwindex, 1);
    uint16_t ctrl = cfg16(0, 0x62);
    ASSERT_TRUE(ctrl & 0x8000);      /* enabled */
    ASSERT_FALSE(ctrl & 0x4000);     /* function mask cleared */
    for (int i = 0; i < 4; i++) ASSERT_EQ(table_entry(i)[3] & 1, 1);   /* every entry masked */
    ASSERT_TRUE(cfg16(0, 0x04) & PCI_CMD_INTX_DISABLE);
    ASSERT_TRUE(cfg16(0, 0x04) & PCI_CMD_MEM_SPACE);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 2, PCI_IRQ_ALL), -EBUSY);

    /* chip ops: message into the entry, mask bit toggles */
    IrqDesc *q = d->dev.irqs[1];
    q->chip->write_msg(q, 0xFEE01000ULL, 0x41);
    ASSERT_EQ(table_entry(1)[0], 0xFEE01000);
    ASSERT_EQ(table_entry(1)[1], 0);
    ASSERT_EQ(table_entry(1)[2], 0x41);
    q->chip->unmask(q);
    ASSERT_EQ(table_entry(1)[3] & 1, 0);
    ASSERT_EQ(table_entry(0)[3] & 1, 1);
    q->chip->mask(q);
    ASSERT_EQ(table_entry(1)[3] & 1, 1);

    uint16_t entries, masked;
    uint8_t bar;
    uint32_t off;
    ASSERT_EQ(pci_msix_info(d, &entries, &bar, &off, &masked), 0);
    ASSERT_EQ(entries, 4);
    ASSERT_EQ(bar, 1);
    ASSERT_EQ(off, 0x800);
    ASSERT_EQ(masked, 4);

    pci_free_irq_vectors(d);
    ASSERT_FALSE(cfg16(0, 0x62) & 0x8000);
    ASSERT_TRUE(d->dev.irqs == NULL);
    ASSERT_EQ(d->irq_kind, 0);
    ASSERT_EQ(frees, 2);
    ASSERT_EQ(descs_live, 0);
    return 0;
}

/* More entries asked for than the table has: the table size wins; fewer than
 * min falls through to the next kind */
static int test_pci_msix_limits(void) {
    PciDevice *d = irq_device(1, 0, 0);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 16, PCI_IRQ_MSIX), 4);
    pci_free_irq_vectors(d);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 5, 8, PCI_IRQ_MSIX), -ENODEV);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 3, 2, PCI_IRQ_MSIX), -EINVAL);
    return 0;
}

/* Without the APIC (PIC mode) MSI-X and MSI are skipped for INTx */
static int test_pci_intx_fallback(void) {
    PciDevice *d = irq_device(1, 1, 0);
    msi_available = 0;
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 2, PCI_IRQ_ALL), 1);
    ASSERT_EQ(d->irq_kind, IRQ_KIND_INTX);
    ASSERT_EQ(d->dev.irqs[0]->line, 11);
    ASSERT_STR_EQ(d->dev.irqs[0]->chip->name, "INTx");
    ASSERT_FALSE(cfg16(0, 0x62) & 0x8000);                  /* MSI-X untouched */
    ASSERT_TRUE(cfg16(0, 0x04) & PCI_CMD_INTX_DISABLE);     /* masked until requested */
    IrqDesc *x = d->dev.irqs[0];
    x->chip->unmask(x);
    ASSERT_FALSE(cfg16(0, 0x04) & PCI_CMD_INTX_DISABLE);
    x->chip->mask(x);
    ASSERT_TRUE(cfg16(0, 0x04) & PCI_CMD_INTX_DISABLE);
    pci_free_irq_vectors(d);

    /* no pin, or a line the firmware didn't set: nothing */
    fake_slots[0].config[15] = 0x000000FF | (1u << 8);
    pci_init();
    d = pci_get_device(0);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 1, PCI_IRQ_INTX), -ENODEV);
    fake_slots[0].config[15] = 0x0000000B;
    pci_init();
    d = pci_get_device(0);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 1, PCI_IRQ_INTX), -ENODEV);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 2, 2, PCI_IRQ_INTX), -ENODEV);
    return 0;
}

/* MSI, 64-bit, per-vector masking: the mask bit masks; MSI turns on at the
 * first unmask, after the message is written */
static int test_pci_msi_pvm(void) {
    PciDevice *d = irq_device(0, 1, 0x0180 | 0x0010);   /* 64-bit, PVM, MME set by firmware */
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 4, PCI_IRQ_MSI | PCI_IRQ_MSIX), 1);
    ASSERT_EQ(d->irq_kind, IRQ_KIND_MSI);
    ASSERT_EQ(cfg16(0, 0x42) & 0x0071, 0);   /* off, MME cleared */
    ASSERT_TRUE(cfg16(0, 0x04) & PCI_CMD_INTX_DISABLE);
    IrqDesc *m = d->dev.irqs[0];
    m->chip->mask(m);
    ASSERT_EQ(cfg(0, 0x50) & 1, 1);           /* mask register after the 64-bit data */
    m->chip->write_msg(m, 0xFEE02000ULL, 0x52);
    ASSERT_EQ(cfg(0, 0x44), 0xFEE02000);
    ASSERT_EQ(cfg(0, 0x48), 0);
    ASSERT_EQ(cfg16(0, 0x4C), 0x52);
    m->chip->unmask(m);
    ASSERT_EQ(cfg(0, 0x50) & 1, 0);
    ASSERT_EQ(cfg16(0, 0x42) & 1, 1);
    m->chip->mask(m);
    ASSERT_EQ(cfg16(0, 0x42) & 1, 1);          /* PVM: MSI stays on, the bit masks */
    pci_free_irq_vectors(d);
    ASSERT_EQ(cfg16(0, 0x42) & 1, 0);
    return 0;
}

/* MSI, 32-bit, no per-vector mask: masking turns MSI off */
static int test_pci_msi_plain(void) {
    PciDevice *d = irq_device(0, 1, 0);
    ASSERT_EQ(pci_alloc_irq_vectors(d, 1, 1, PCI_IRQ_ALL), 1);
    IrqDesc *m = d->dev.irqs[0];
    m->chip->write_msg(m, 0xFEE00000ULL, 0x33);
    ASSERT_EQ(cfg(0, 0x44), 0xFEE00000);
    ASSERT_EQ(cfg16(0, 0x48), 0x33);
    ASSERT_EQ(cfg16(0, 0x42) & 1, 0);
    m->chip->unmask(m);
    ASSERT_EQ(cfg16(0, 0x42) & 1, 1);
    m->chip->mask(m);
    ASSERT_EQ(cfg16(0, 0x42) & 1, 0);
    return 0;
}

/* The report reads tables only where decoding is already on */
static int test_pci_msix_info_decode_off(void) {
    PciDevice *d = irq_device(1, 0, 0);
    uint16_t entries, masked;
    uint8_t bar;
    uint32_t off;
    ASSERT_EQ(pci_msix_info(d, &entries, &bar, &off, &masked), -1);
    ASSERT_EQ(entries, 4);
    ASSERT_EQ(mmio_maps, 0);
    ASSERT_FALSE(cfg16(0, 0x04) & PCI_CMD_MEM_SPACE);   /* not switched on */
    PciDevice plain;
    memset(&plain, 0, sizeof(plain));
    ASSERT_EQ(pci_msix_info(&plain, &entries, &bar, &off, &masked), -1);
    return 0;
}

TestCase pci_tests[] = {
    {"pci_empty_bus", test_pci_empty_bus},
    {"pci_single_device", test_pci_single_device},
    {"pci_multifunction", test_pci_multifunction},
    {"pci_multifunction_after_table_full", test_pci_multifunction_after_table_full},
    {"pci_class_names", test_pci_class_names},
    {"pci_bar_io_base", test_pci_bar_io_base},
    {"pci_get_device_bounds", test_pci_get_device_bounds},
    {"pci_bar_sizing", test_pci_bar_sizing},
    {"pci_bar_sizing_decode_off", test_pci_bar_sizing_decode_off},
    {"pci_enable_bits", test_pci_enable_bits},
    {"pci_capabilities", test_pci_capabilities},
    {"pci_capability_loop_and_absent", test_pci_capability_loop_and_absent},
    {"pci_match_id", test_pci_match_id},
    {"pci_driver_binding", test_pci_driver_binding},
    {"pci_msix_alloc", test_pci_msix_alloc},
    {"pci_msix_limits", test_pci_msix_limits},
    {"pci_intx_fallback", test_pci_intx_fallback},
    {"pci_msi_pvm", test_pci_msi_pvm},
    {"pci_msi_plain", test_pci_msi_plain},
    {"pci_msix_info_decode_off", test_pci_msix_info_decode_off},
};

int pci_test_count = sizeof(pci_tests) / sizeof(pci_tests[0]);
