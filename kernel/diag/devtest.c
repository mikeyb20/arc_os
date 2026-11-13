/* arc_os — Driver-model test device (M1-09, test mode only)
 *
 * Owns: a driver for QEMU's "edu" teaching device (1234:11e8), registered only
 * by the QEMU suite "devices" (arc.test=devices). It proves the parts of the
 * driver model the virtio drivers don't reach: binding by ID table, MSI (edu
 * has MSI but no MSI-X), INTx with arc.devtest=intx, moving an interrupt to
 * another CPU, and a DMA round trip through both DMA helpers (a coherent
 * buffer to the device, the device into a page mapped for streaming).
 * With virtio-net bound it also asks QEMU's gateway for its MAC (ARP) and
 * waits for the reply's RX interrupt, so the network path is checked end to
 * end before any program runs. Results are "[DEVTEST] ..." lines the suite
 * matches.
 * Invariants: runs on the BSP during boot, before the scheduler starts; waits
 * are bounded by TSC time (TCG and KVM alike) with interrupts let in.
 * Locks: none; the handler only touches the device and counters. The
 * network check runs before the scheduler and the other CPUs do anything, so
 * it needs no kernel lock.
 * Cost: test mode only. */

#include "diag/diag.h"
#include "drivers/pci.h"
#include "drivers/irq.h"
#include "drivers/dma.h"
#include "drivers/mmio.h"
#include "arch/hal.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "mm/gfp.h"
#include "lib/kprintf.h"
#include "lib/cmdline.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "uapi/errno.h"
#include "drivers/virtio_net.h"
#include "net/netif.h"
#include "net/arp.h"
#include "net/ethernet.h"
#include "net/net_util.h"

/* edu registers (QEMU docs/specs/edu.rst) */
#define EDU_ID          0x00
#define EDU_LIVENESS    0x04
#define EDU_IRQ_STATUS  0x24
#define EDU_IRQ_RAISE   0x60
#define EDU_IRQ_ACK     0x64
#define EDU_DMA_SRC     0x80
#define EDU_DMA_DST     0x88
#define EDU_DMA_COUNT   0x90
#define EDU_DMA_CMD     0x98
#define EDU_DMA_START   0x1
#define EDU_DMA_TO_RAM  0x2   /* direction: device buffer -> RAM */
#define EDU_DMA_IRQ     0x4   /* raise interrupt 0x100 when done */
#define EDU_DMA_IRQ_BIT 0x100
#define EDU_BUFFER      0x40000   /* the device's 4 KiB buffer, in its own address space */
#define EDU_BUFFER_SIZE 4096

#define WAIT_MS 2000

static volatile uint8_t *edu;
static volatile uint64_t irq_count;
static volatile uint32_t last_status;

static uint32_t rd(uint32_t reg) {
    return mmio_read32(edu + reg);
}

static void wr(uint32_t reg, uint32_t v) {
    mmio_write32(edu + reg, v);
}

static void wr64(uint32_t reg, uint64_t v) {
    mmio_write64(edu + reg, v);
}

static int edu_irq(void *arg) {
    (void)arg;
    uint32_t status = rd(EDU_IRQ_STATUS);
    if (status == 0) return IRQ_NONE;   /* shared INTx: not ours */
    wr(EDU_IRQ_ACK, status);
    last_status = status;
    __atomic_add_fetch(&irq_count, 1, __ATOMIC_RELEASE);
    return IRQ_HANDLED;
}

/* Wait with interrupts on until *counter differs from before, at most
 * WAIT_MS of TSC time. Returns 1 if it changed. */
static int wait_change(const volatile uint64_t *counter, uint64_t before) {
    uint64_t limit = diag_ms_to_cycles(WAIT_MS);
    uint64_t flags = irq_save();
    hal_enable_interrupts();
    uint64_t start = hal_tsc_read();
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) == before && hal_tsc_read() - start < limit) {
        hal_cpu_relax();
    }
    irq_restore(flags);
    return __atomic_load_n(counter, __ATOMIC_ACQUIRE) != before;
}

static int wait_irq(uint64_t before) {
    return wait_change(&irq_count, before);
}

static int raise_and_wait(void) {
    uint64_t before = irq_count;
    wr(EDU_IRQ_RAISE, 1);
    return wait_irq(before);
}

/* One DMA transfer, ended by the device's interrupt. */
static int edu_dma(uint64_t src, uint64_t dst, uint32_t count, uint32_t dir) {
    uint64_t before = irq_count;
    wr64(EDU_DMA_SRC, src);
    wr64(EDU_DMA_DST, dst);
    wr64(EDU_DMA_COUNT, count);
    wr64(EDU_DMA_CMD, EDU_DMA_START | dir | EDU_DMA_IRQ);
    return wait_irq(before) && (last_status & EDU_DMA_IRQ_BIT) && !(rd(EDU_DMA_CMD) & EDU_DMA_START);
}

static void test_affinity(Device *dev) {
    int from = irq_get_cpu(dev, 0);
    uint32_t to = from < 0 ? 0 : (uint32_t)from;
    for (uint32_t i = 1; i <= irq_cpu_slots(); i++) {
        uint32_t c = ((uint32_t)(from < 0 ? 0 : from) + i) % irq_cpu_slots();
        if (hal_irq_msi_target_ok(c) && (int)c != from) {
            to = c;
            break;
        }
    }
    int rc = irq_set_affinity(dev, 0, to);
    if (rc != 0) {
        kprintf("[DEVTEST] edu: affinity cpu%d -> cpu%u refused (%d)\n", from, to, rc);
        return;
    }
    uint64_t before = dev->irqs[0]->counts[to];
    int ok = raise_and_wait() && dev->irqs[0]->counts[to] == before + 1;
    kprintf("[DEVTEST] edu: affinity cpu%d -> cpu%u %s\n", from, to, ok ? "ok" : "FAILED");
}

/* Round trip RAM -> edu -> RAM. Out of a coherent buffer (the 32-bit mask
 * puts it below 4 GiB); back into a page mapped for streaming through its
 * kernel (HHDM) address, so dma_map_single's page-table walk runs for real. */
static void test_dma(PciDevice *pdev) {
    Device *dev = &pdev->dev;
    uint64_t src_bus = 0;
    uint8_t *src = dma_alloc_coherent(dev, EDU_BUFFER_SIZE, &src_bus, 0);
    struct page *pg = alloc_pages(0, GFP_ZERO | GFP_DMA32);
    if (src == NULL || pg == NULL) {
        kprintf("[DEVTEST] edu: dma FAILED (out of memory)\n");
        if (src != NULL) dma_free_coherent(dev, EDU_BUFFER_SIZE, src, src_bus);
        if (pg != NULL) free_pages(pg, 0);
        return;
    }
    uint8_t *dst = page_to_virt(pg);
    for (int i = 0; i < EDU_BUFFER_SIZE; i++) src[i] = (uint8_t)(i * 7 + 3);
    uint64_t dst_bus = dma_map_single(dev, dst, EDU_BUFFER_SIZE, DMA_FROM_DEVICE);
    int ok = dst_bus != DMA_MAPPING_ERROR && dst_bus == page_to_phys(pg) &&
             edu_dma(src_bus, EDU_BUFFER, EDU_BUFFER_SIZE, 0) &&
             edu_dma(EDU_BUFFER, dst_bus, EDU_BUFFER_SIZE, EDU_DMA_TO_RAM);
    if (dst_bus != DMA_MAPPING_ERROR) dma_unmap(dev, dst_bus, EDU_BUFFER_SIZE, DMA_FROM_DEVICE);
    ok = ok && memcmp(src, dst, EDU_BUFFER_SIZE) == 0;
    kprintf("[DEVTEST] edu: dma %u bytes round trip %s (out of 0x%lx, back into 0x%lx)\n", EDU_BUFFER_SIZE,
            ok ? "ok" : "FAILED", src_bus, dst_bus);
    dma_free_coherent(dev, EDU_BUFFER_SIZE, src, src_bus);
    free_pages(pg, 0);
}

static int edu_probe(PciDevice *pdev, const PciDeviceId *id) {
    (void)id;
    edu = (volatile uint8_t *)pci_bar_map(pdev, 0);
    if (edu == NULL) return -1;
    wr(EDU_LIVENESS, 0x12345678);
    kprintf("[DEVTEST] edu: id 0x%x, liveness %s, BAR0 %lu KiB\n", rd(EDU_ID),
            rd(EDU_LIVENESS) == ~0x12345678u ? "ok" : "FAILED", pdev->bar_size[0] / 1024);

    char mode[8];
    int intx = cmdline_get(cmdline_raw(), "arc.devtest", mode, sizeof(mode)) && strcmp(mode, "intx") == 0;
    /* edu reaches 28 bits by default; the suite raises it to 32 (-device
     * edu,dma_mask=0xffffffff). Not more: QEMU 8.2 splits a 64-bit write of
     * its DMA address registers and drops the upper half. */
    if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(32)) != 0) return -1;
    int n = pci_alloc_irq_vectors(pdev, 1, 1, intx ? PCI_IRQ_INTX : PCI_IRQ_ALL);
    if (n < 1 || irq_request(&pdev->dev, 0, edu_irq, NULL, IRQF_SHARED, "edu") != 0) {
        kprintf("[DEVTEST] edu: no interrupt (%d)\n", n);
        return -1;
    }
    pci_enable_bus_master(pdev);
    const char *kind = pdev->irq_kind == IRQ_KIND_MSI ? "msi" : (pdev->irq_kind == IRQ_KIND_INTX ? "intx" : "msix");
    int ok = raise_and_wait();
    kprintf("[DEVTEST] edu: %s interrupt on cpu%d %s\n", kind, irq_get_cpu(&pdev->dev, 0), ok ? "ok" : "FAILED");
    if (pdev->irq_kind != IRQ_KIND_INTX) test_affinity(&pdev->dev);
    test_dma(pdev);
    return 0;
}

static int edu_suspend(Device *dev, int how) {
    (void)dev;
    (void)how;
    return -EOPNOTSUPP;
}

static const PciDeviceId edu_ids[] = {
    PCI_DEVICE(0x1234, 0x11E8),
    {0},
};

static PciDriver edu_driver = {
    .driver = {.name = "edu-test", .suspend = edu_suspend, .resume = edu_suspend},
    .ids = edu_ids,
    .probe = edu_probe,
};

/* ARP request for QEMU's gateway (10.0.2.2), then its reply: TX, the RX
 * interrupt, and the receive path into the stack. */
static void test_net(void) {
    NetIf *nif = netif_get_default();
    if (nif == NULL || nif->gateway == 0) {   /* no virtio-net: only loopback */
        kprintf("[DEVTEST] net: no network device\n");
        return;
    }
    uint32_t gw = nif->gateway;
    ArpPacket req;
    memset(&req, 0, sizeof(req));
    req.htype = htons(ARP_HTYPE_ETHER);
    req.ptype = htons(ARP_PTYPE_IPV4);
    req.hlen = ETH_ALEN;
    req.plen = 4;
    req.oper = htons(ARP_OP_REQUEST);
    memcpy(req.sha, nif->mac, ETH_ALEN);
    req.spa = nif->ip_addr;
    req.tpa = gw;
    static const uint8_t broadcast[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    uint64_t first = virtio_net_rx_kicks();
    uint64_t before = first;
    if (eth_send(nif, broadcast, ETH_TYPE_ARP, &req, ARP_PACKET_SIZE) != 0) {
        kprintf("[DEVTEST] net: ARP request not sent FAILED\n");
        return;
    }
    /* The RX interrupt only counts and wakes the worker, which isn't running
     * yet: receive by hand each time the count moves */
    uint64_t limit = diag_ms_to_cycles(WAIT_MS);
    uint64_t start = hal_tsc_read();
    int got = 0;
    while (!got && hal_tsc_read() - start < limit) {
        uint64_t flags = irq_save();
        hal_enable_interrupts();
        hal_cpu_relax();
        irq_restore(flags);
        if (virtio_net_rx_kicks() != before) {
            virtio_net_poll_rx();
            got = arp_lookup(gw) != NULL;
            before = virtio_net_rx_kicks();
        }
    }
    kprintf("[DEVTEST] net: ARP reply from the gateway after %lu RX interrupt(s) %s\n", virtio_net_rx_kicks() - first,
            got ? "ok" : "FAILED");
}

void devtest_register(void) {
    if (pci_register_driver(&edu_driver) == 0) kprintf("[DEVTEST] edu: no device bound\n");
    test_net();
}
