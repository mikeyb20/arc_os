/* arc_os — Host-side tests for kernel/drivers/virtio.c virtqueue logic */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_ARCH_X86_64_IO_H
#define ARCHOS_MM_PMM_H
#define ARCHOS_MM_VMM_H
#define ARCHOS_DRIVERS_PCI_H
#define ARCHOS_LIB_MEM_H  /* Use libc memset/memcpy */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Stub I/O */
static void outb(uint16_t port, uint8_t value) { (void)port; (void)value; }
static uint8_t inb(uint16_t port) { (void)port; return 0; }
static void outw(uint16_t port, uint16_t value) { (void)port; (void)value; }
static uint16_t inw(uint16_t port) { (void)port; return 0; }
static void outl(uint16_t port, uint32_t value) { (void)port; (void)value; }
static uint32_t inl(uint16_t port) { (void)port; return 0; }

/* Stub PMM */
#define PAGE_SIZE 4096
static uint8_t fake_pmm_arena[PAGE_SIZE * 16] __attribute__((aligned(4096)));
static int fake_pmm_next_page;

static uint64_t pmm_alloc_page(void) {
    if (fake_pmm_next_page >= 16) return 0;
    return (uint64_t)(uintptr_t)&fake_pmm_arena[PAGE_SIZE * fake_pmm_next_page++];
}

static uint64_t pmm_alloc_contiguous(size_t count) {
    if (fake_pmm_next_page + (int)count > 16) return 0;
    uint64_t base = (uint64_t)(uintptr_t)&fake_pmm_arena[PAGE_SIZE * fake_pmm_next_page];
    fake_pmm_next_page += (int)count;
    return base;
}

static void pmm_free_page(uint64_t phys_addr) { (void)phys_addr; }

/* Stub VMM — HHDM offset is 0 since we use real pointers in host tests */
static uint64_t vmm_get_hhdm_offset(void) { return 0; }

/* Reproduce PCI types (since we guard pci.h) */
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

static uint16_t pci_bar_io_base(uint32_t bar) {
    return (uint16_t)(bar & 0xFFFC);
}

static void pci_enable_bus_master(const PciDevice *dev) { (void)dev; }

/* Include the real VirtIO implementation */
#include "../kernel/drivers/virtio.c"

/* --- Test helpers --- */

static void reset_pmm(void) {
    fake_pmm_next_page = 0;
    memset(fake_pmm_arena, 0, sizeof(fake_pmm_arena));
}

/* Create a Virtqueue manually for testing descriptor logic */
static VringDesc test_desc[16];
static uint8_t test_avail_buf[256];
static uint8_t test_used_buf[256];

static Virtqueue make_test_vq(uint16_t size) {
    memset(test_desc, 0, sizeof(test_desc));
    memset(test_avail_buf, 0, sizeof(test_avail_buf));
    memset(test_used_buf, 0, sizeof(test_used_buf));

    Virtqueue vq;
    memset(&vq, 0, sizeof(vq));
    vq.size = size;
    vq.desc = test_desc;
    vq.avail = (VringAvail *)test_avail_buf;
    vq.used = (VringUsed *)test_used_buf;
    vq.last_used_idx = 0;

    /* Initialize free list */
    for (uint16_t i = 0; i < size; i++) {
        vq.desc[i].next = (i + 1 < size) ? (i + 1) : VRING_DESC_NONE;
    }
    vq.free_head = 0;
    vq.num_free = size;

    return vq;
}

/* --- Tests --- */

static int test_virtq_alloc_desc_basic(void) {
    Virtqueue vq = make_test_vq(4);
    ASSERT_EQ(vq.num_free, 4);

    uint16_t d0 = virtq_alloc_desc(&vq);
    ASSERT_EQ(d0, 0);
    ASSERT_EQ(vq.num_free, 3);

    uint16_t d1 = virtq_alloc_desc(&vq);
    ASSERT_EQ(d1, 1);
    ASSERT_EQ(vq.num_free, 2);

    uint16_t d2 = virtq_alloc_desc(&vq);
    ASSERT_EQ(d2, 2);

    uint16_t d3 = virtq_alloc_desc(&vq);
    ASSERT_EQ(d3, 3);
    ASSERT_EQ(vq.num_free, 0);

    /* All exhausted */
    uint16_t d4 = virtq_alloc_desc(&vq);
    ASSERT_EQ(d4, VRING_DESC_NONE);

    return 0;
}

static int test_virtq_free_chain_single(void) {
    Virtqueue vq = make_test_vq(4);
    uint16_t d = virtq_alloc_desc(&vq);
    ASSERT_EQ(vq.num_free, 3);

    virtq_free_chain(&vq, d);
    ASSERT_EQ(vq.num_free, 4);

    /* Can alloc again */
    uint16_t d2 = virtq_alloc_desc(&vq);
    ASSERT_NEQ(d2, VRING_DESC_NONE);
    return 0;
}

static int test_virtq_free_chain_multi(void) {
    Virtqueue vq = make_test_vq(8);

    /* Build a 3-descriptor chain */
    uint16_t d0 = virtq_alloc_desc(&vq);
    uint16_t d1 = virtq_alloc_desc(&vq);
    uint16_t d2 = virtq_alloc_desc(&vq);
    ASSERT_EQ(vq.num_free, 5);

    vq.desc[d0].flags = VRING_DESC_F_NEXT;
    vq.desc[d0].next = d1;
    vq.desc[d1].flags = VRING_DESC_F_NEXT;
    vq.desc[d1].next = d2;
    vq.desc[d2].flags = 0;
    vq.desc[d2].next = VRING_DESC_NONE;

    /* Free the chain */
    virtq_free_chain(&vq, d0);
    ASSERT_EQ(vq.num_free, 8);
    return 0;
}

static int test_virtq_alloc_after_free(void) {
    Virtqueue vq = make_test_vq(2);

    uint16_t d0 = virtq_alloc_desc(&vq);
    uint16_t d1 = virtq_alloc_desc(&vq);
    ASSERT_EQ(vq.num_free, 0);

    virtq_free_chain(&vq, d0);
    ASSERT_EQ(vq.num_free, 1);

    uint16_t d2 = virtq_alloc_desc(&vq);
    ASSERT_EQ(d2, d0);  /* Should reuse freed descriptor */
    ASSERT_EQ(vq.num_free, 0);

    virtq_free_chain(&vq, d1);
    virtq_free_chain(&vq, d2);
    ASSERT_EQ(vq.num_free, 2);
    return 0;
}

static int test_virtq_has_used_empty(void) {
    Virtqueue vq = make_test_vq(4);
    /* No used entries */
    ASSERT_FALSE(virtq_has_used(&vq));
    return 0;
}

static int test_virtq_has_used_after_device(void) {
    Virtqueue vq = make_test_vq(4);
    /* Simulate device placing a used entry */
    vq.used->ring[0].id = 0;
    vq.used->ring[0].len = 512;
    vq.used->idx = 1;

    ASSERT_TRUE(virtq_has_used(&vq));

    uint32_t len;
    uint16_t head = virtq_pop_used(&vq, &len);
    ASSERT_EQ(head, 0);
    ASSERT_EQ(len, 512);

    /* Now consumed */
    ASSERT_FALSE(virtq_has_used(&vq));
    return 0;
}

static int test_virtq_pop_used_multiple(void) {
    Virtqueue vq = make_test_vq(4);
    /* Simulate device completing 2 requests */
    vq.used->ring[0].id = 2;
    vq.used->ring[0].len = 256;
    vq.used->ring[1].id = 5;
    vq.used->ring[1].len = 1024;
    vq.used->idx = 2;

    ASSERT_TRUE(virtq_has_used(&vq));

    uint32_t len;
    uint16_t h0 = virtq_pop_used(&vq, &len);
    ASSERT_EQ(h0, 2);
    ASSERT_EQ(len, 256);

    ASSERT_TRUE(virtq_has_used(&vq));

    uint16_t h1 = virtq_pop_used(&vq, &len);
    ASSERT_EQ(h1, 5);
    ASSERT_EQ(len, 1024);

    ASSERT_FALSE(virtq_has_used(&vq));
    return 0;
}

static int test_virtio_init_queue(void) {
    reset_pmm();

    /* Set up a fake PCI device with an I/O BAR */
    PciDevice fake_pci;
    memset(&fake_pci, 0, sizeof(fake_pci));
    fake_pci.bar[0] = 0xC041;  /* I/O space, base 0xC040 */

    VirtioDevice vdev;
    memset(&vdev, 0, sizeof(vdev));
    vdev.pci = &fake_pci;
    vdev.io_base = pci_bar_io_base(fake_pci.bar[0]);
    vdev.num_queues = 0;

    /* virtio_init_queue reads queue size via inw — our stub returns 0,
     * so queue will be reported as "not available". This validates the
     * error path. */
    int ret = virtio_init_queue(&vdev, 0);
    ASSERT_EQ(ret, -1);  /* Queue size 0 → not available */
    return 0;
}

/* --- Test suite export --- */

TestCase virtio_tests[] = {
    { "virtq_alloc_desc_basic",   test_virtq_alloc_desc_basic },
    { "virtq_free_chain_single",  test_virtq_free_chain_single },
    { "virtq_free_chain_multi",   test_virtq_free_chain_multi },
    { "virtq_alloc_after_free",   test_virtq_alloc_after_free },
    { "virtq_has_used_empty",     test_virtq_has_used_empty },
    { "virtq_has_used_after_device", test_virtq_has_used_after_device },
    { "virtq_pop_used_multiple",  test_virtq_pop_used_multiple },
    { "virtio_init_queue",        test_virtio_init_queue },
};

int virtio_test_count = sizeof(virtio_tests) / sizeof(virtio_tests[0]);
