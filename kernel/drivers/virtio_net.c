#include "drivers/virtio_net.h"
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "net/netif.h"
#include "net/ethernet.h"

#define RX_BUF_COUNT  16
#define POLL_TIMEOUT  10000000
#define RX_QUEUE  0
#define TX_QUEUE  1

static VirtioDevice net_vdev;
static int net_initialized;
static uint8_t net_mac[6];

/* RX buffer tracking */
typedef struct {
    uint64_t phys;
    void    *virt;
    uint16_t desc_idx;
} NetRxBuf;

static NetRxBuf rx_bufs[RX_BUF_COUNT];

/* NetIf for the network stack */
static NetIf net_nif;

/* Forward declaration for rx dispatch */
static void net_rx_dispatch(const void *frame, uint32_t len);

/* --- Send wrapper for NetIf --- */

static int virtio_net_nif_send(NetIf *nif, const void *frame, uint32_t len) {
    (void)nif;
    return virtio_net_send(frame, len);
}

/* --- RX buffer management --- */

static int rx_buf_submit(int idx) {
    Virtqueue *vq = &net_vdev.queues[RX_QUEUE];
    uint16_t d = virtq_alloc_desc(vq);
    if (d == VRING_DESC_NONE) return -1;

    rx_bufs[idx].desc_idx = d;
    vq->desc[d].addr = rx_bufs[idx].phys;
    vq->desc[d].len = PAGE_SIZE;
    vq->desc[d].flags = VRING_DESC_F_WRITE;  /* device writes into this */
    vq->desc[d].next = VRING_DESC_NONE;

    virtio_submit(&net_vdev, RX_QUEUE, d);
    return 0;
}

static int rx_bufs_init(void) {
    uint64_t hhdm = vmm_get_hhdm_offset();
    for (int i = 0; i < RX_BUF_COUNT; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) return -1;
        rx_bufs[i].phys = phys;
        rx_bufs[i].virt = (void *)(phys + hhdm);
        memset(rx_bufs[i].virt, 0, PAGE_SIZE);
        if (rx_buf_submit(i) != 0) return -1;
    }
    return 0;
}

/* --- IRQ handler --- */

static void virtio_net_irq_handler(InterruptFrame *frame) {
    (void)frame;
    /* Reading ISR status clears the interrupt */
    inb(net_vdev.io_base + VIRTIO_REG_ISR_STATUS);
    virtio_net_poll_rx();
}

/* --- Public API --- */

int virtio_net_init(void) {
    const PciDevice *pci = pci_find_device(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID);
    if (!pci) {
        kprintf("[VIRTIO-NET] No VirtIO network device found\n");
        return -1;
    }

    kprintf("[VIRTIO-NET] Found device at %x:%x.%x (IRQ %d)\n",
            pci->addr.bus, pci->addr.device, pci->addr.function, pci->irq_line);

    if (virtio_init_device(&net_vdev, pci) != 0) return -1;

    /* Accept MAC feature only */
    virtio_negotiate_features(&net_vdev, VIRTIO_NET_F_MAC);

    /* Initialize receiveq (0) and transmitq (1) */
    if (virtio_init_queue(&net_vdev, RX_QUEUE) != 0) return -1;
    if (virtio_init_queue(&net_vdev, TX_QUEUE) != 0) return -1;

    /* Read MAC from device-specific config */
    for (int i = 0; i < 6; i++)
        net_mac[i] = inb(net_vdev.io_base + VIRTIO_REG_CONFIG + i);

    kprintf("[VIRTIO-NET] MAC: %x:%x:%x:%x:%x:%x\n",
            net_mac[0], net_mac[1], net_mac[2],
            net_mac[3], net_mac[4], net_mac[5]);

    virtio_device_ready(&net_vdev);

    /* Pre-fill RX buffers */
    if (rx_bufs_init() != 0) {
        kprintf("[VIRTIO-NET] Failed to allocate RX buffers\n");
        return -1;
    }

    /* Register IRQ handler */
    uint8_t irq = pci->irq_line;
    isr_register_handler(IRQ_BASE + irq, virtio_net_irq_handler);
    pic_unmask(irq);

    /* Register with network interface layer */
    memcpy(net_nif.mac, net_mac, 6);
    net_nif.send = virtio_net_nif_send;
    netif_register(&net_nif);

    net_initialized = 1;
    kprintf("[VIRTIO-NET] Initialized (%d RX buffers)\n", RX_BUF_COUNT);
    return 0;
}

int virtio_net_send(const void *data, uint32_t len) {
    if (!net_initialized) return -1;

    Virtqueue *vq = &net_vdev.queues[TX_QUEUE];
    uint64_t hhdm = vmm_get_hhdm_offset();

    /* Allocate a page for virtio-net header + frame */
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) return -1;
    void *buf = (void *)(phys + hhdm);

    /* Zero the virtio-net header */
    memset(buf, 0, VIRTIO_NET_HDR_SIZE);
    /* Copy ethernet frame after header */
    memcpy((uint8_t *)buf + VIRTIO_NET_HDR_SIZE, data, len);

    /* Build single descriptor (device-readable) */
    uint16_t d = virtq_alloc_desc(vq);
    if (d == VRING_DESC_NONE) {
        pmm_free_page(phys);
        return -1;
    }
    vq->desc[d].addr = phys;
    vq->desc[d].len = VIRTIO_NET_HDR_SIZE + len;
    vq->desc[d].flags = 0;  /* device-readable, no next */
    vq->desc[d].next = VRING_DESC_NONE;

    virtio_submit(&net_vdev, TX_QUEUE, d);

    /* Poll for completion */
    int timeout = POLL_TIMEOUT;
    while (!virtq_has_used(vq) && --timeout > 0)
        __asm__ volatile ("pause");

    uint32_t used_len;
    if (timeout > 0)
        virtq_pop_used(vq, &used_len);

    virtq_free_chain(vq, d);
    pmm_free_page(phys);

    return (timeout > 0) ? 0 : -1;
}

int virtio_net_poll_rx(void) {
    if (!net_initialized) return 0;

    Virtqueue *vq = &net_vdev.queues[RX_QUEUE];
    int count = 0;

    while (virtq_has_used(vq)) {
        uint32_t used_len;
        uint16_t desc_head = virtq_pop_used(vq, &used_len);

        /* Find matching RX buffer */
        int buf_idx = -1;
        for (int i = 0; i < RX_BUF_COUNT; i++) {
            if (rx_bufs[i].desc_idx == desc_head) {
                buf_idx = i;
                break;
            }
        }
        if (buf_idx < 0) {
            virtq_free_chain(vq, desc_head);
            continue;
        }

        /* Skip virtio-net header, dispatch ethernet frame */
        if (used_len > VIRTIO_NET_HDR_SIZE) {
            const void *frame = (uint8_t *)rx_bufs[buf_idx].virt + VIRTIO_NET_HDR_SIZE;
            uint32_t frame_len = used_len - VIRTIO_NET_HDR_SIZE;
            net_rx_dispatch(frame, frame_len);
        }

        /* Re-submit buffer */
        virtq_free_chain(vq, desc_head);
        memset(rx_bufs[buf_idx].virt, 0, PAGE_SIZE);
        rx_buf_submit(buf_idx);
        count++;
    }
    return count;
}

void virtio_net_get_mac(uint8_t mac[6]) {
    memcpy(mac, net_mac, 6);
}

static void net_rx_dispatch(const void *frame, uint32_t len) {
    NetIf *nif = netif_get_default();
    if (nif)
        eth_rx(nif, frame, len);
}
