/* arc_os — virtio-net driver (legacy virtio-PCI)
 *
 * Owns: one virtio network device (the first one found), its RX buffers, its
 * RX worker thread and its NetIf.
 * Invariants: the interrupt handler only acknowledges the device and wakes
 * the RX worker; frames reach the network stack from the worker thread, so
 * nothing allocates in hard-IRQ context (work-blocks §7). TX is polled and
 * raises no interrupts (VRING_AVAIL_F_NO_INTERRUPT, no MSI-X vector).
 * Locks: rx_lock (LOCK_LEVEL_OBJECT, IRQ-safe) guards rx_pending and is the
 * worker's wq_sleep lock. The RX and TX queues and the network stack run under
 * the big kernel lock (syscalls, and the worker takes it) until M1-18.
 * Cost: one wakeup per interrupt; the worker drains every used buffer per
 * wakeup. RX buffer lookup by descriptor is O(RX_BUF_COUNT) (16). */

#include "drivers/virtio_net.h"
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "drivers/irq.h"
#include "drivers/dma.h"
#include "arch/hal.h"
#include "mm/pmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "net/netif.h"
#include "net/ethernet.h"
#include "proc/kernel_lock.h"
#include "proc/spinlock.h"
#include "proc/waitqueue.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "uapi/errno.h"

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

/* RX worker: woken by the interrupt handler */
static Spinlock rx_lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "virtio-net rx");
static WaitQueue rx_wq = WAITQUEUE_INIT;
static int rx_pending;
static uint64_t rx_wakeups;
static uint64_t rx_kicks;     /* RX interrupts (hard IRQ, atomic) */

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
    for (int i = 0; i < RX_BUF_COUNT; i++) {
        rx_bufs[i].virt = dma_alloc_coherent(&net_vdev.pci->dev, PAGE_SIZE, &rx_bufs[i].phys, 0);
        if (rx_bufs[i].virt == NULL) return -1;
        if (rx_buf_submit(i) != 0) return -1;
    }
    return 0;
}

/* --- Interrupts and the RX worker --- */

/* Hard IRQ: note the work and wake the worker; nothing else. */
static void rx_kick(void) {
    __atomic_add_fetch(&rx_kicks, 1, __ATOMIC_RELAXED);
    uint64_t flags;
    spin_lock_irqsave(&rx_lock, &flags);
    rx_pending = 1;
    wq_wake(&rx_wq);
    spin_unlock_irqrestore(&rx_lock, flags);
}

/* MSI-X entry 1: the RX queue has used buffers. */
static int net_rx_irq(void *arg) {
    (void)arg;
    rx_kick();
    return IRQ_HANDLED;
}

/* MSI-X entry 0: config change (link status); nothing reads it yet. */
static int net_config_irq(void *arg) {
    (void)arg;
    return IRQ_HANDLED;
}

/* INTx (shared line): reading the ISR status acknowledges the device. */
static int net_intx_irq(void *arg) {
    uint8_t isr = virtio_isr_status(arg);
    if (isr & VIRTIO_ISR_QUEUE) rx_kick();
    return isr != 0 ? IRQ_HANDLED : IRQ_NONE;
}

/* Kernel thread: hand received frames to the network stack. The stack has no
 * locks of its own beyond the sockets': it runs under the kernel lock, like
 * the syscalls that use it (M1-05), which is held only with interrupts off. */
static void net_rx_worker(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&rx_lock, &flags);
        while (!rx_pending) {
            wq_sleep(&rx_wq, &rx_lock, flags);
            spin_lock_irqsave(&rx_lock, &flags);
        }
        rx_pending = 0;
        rx_wakeups++;
        spin_unlock(&rx_lock);   /* interrupts stay off for the kernel lock */
        kernel_lock();
        virtio_net_poll_rx();
        kernel_unlock();
        irq_restore(flags);
    }
}

uint64_t virtio_net_rx_wakeups(void) {
    return __atomic_load_n(&rx_wakeups, __ATOMIC_RELAXED);
}

uint64_t virtio_net_rx_kicks(void) {
    return __atomic_load_n(&rx_kicks, __ATOMIC_RELAXED);
}

/* MSI-X entry 0 = config, entry 1 = RX (one vector: RX takes entry 0); TX has
 * none. INTx: one shared handler. TX never interrupts either way. */
static int net_setup_irqs(VirtioDevice *v) {
    Device *dev = &v->pci->dev;
    int n = virtio_setup_irqs(v, 2);
    if (n < 0) return n;
    v->queues[TX_QUEUE].avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
    if (!v->msix) return irq_request(dev, 0, net_intx_irq, v, IRQF_SHARED, "virtio-net");
    uint16_t rxvec = n >= 2 ? 1 : 0;
    int rc = irq_request(dev, rxvec, net_rx_irq, v, 0, "virtio-net-rx");
    if (rc == 0 && n >= 2) rc = irq_request(dev, 0, net_config_irq, v, 0, "virtio-net-config");
    if (rc != 0) return rc;
    if (virtio_set_queue_vector(v, RX_QUEUE, rxvec) != 0 ||
        virtio_set_queue_vector(v, TX_QUEUE, VIRTIO_MSI_NO_VECTOR) != 0 ||
        virtio_set_config_vector(v, n >= 2 ? 0 : VIRTIO_MSI_NO_VECTOR) != 0) {
        kprintf("[VIRTIO-NET] device refused the MSI-X vectors\n");
        return -EIO;
    }
    return 0;
}

/* --- Driver --- */

static int virtio_net_probe(PciDevice *pci, const PciDeviceId *id) {
    (void)id;
    if (net_initialized) return -EBUSY;   /* one virtio NIC for now */

    if (virtio_init_device(&net_vdev, pci) != 0) return -ENODEV;

    /* Accept MAC feature only */
    virtio_negotiate_features(&net_vdev, VIRTIO_NET_F_MAC);

    /* Initialize receiveq (0) and transmitq (1) */
    if (virtio_init_queue(&net_vdev, RX_QUEUE) != 0) return -ENOMEM;
    if (virtio_init_queue(&net_vdev, TX_QUEUE) != 0) return -ENOMEM;

    Thread *worker = thread_create(net_rx_worker, NULL);
    if (worker == NULL) return -ENOMEM;
    int rc = net_setup_irqs(&net_vdev);
    if (rc != 0) {
        thread_destroy(worker);
        return rc;
    }

    /* Read MAC from device-specific config (moved by MSI-X) */
    for (int i = 0; i < 6; i++) net_mac[i] = virtio_config_read8(&net_vdev, (uint16_t)i);

    kprintf("[VIRTIO-NET] MAC: %x:%x:%x:%x:%x:%x\n",
            net_mac[0], net_mac[1], net_mac[2],
            net_mac[3], net_mac[4], net_mac[5]);

    virtio_device_ready(&net_vdev);

    /* Pre-fill RX buffers (the device is live: the worker may run at once) */
    if (rx_bufs_init() != 0) {
        kprintf("[VIRTIO-NET] Failed to allocate RX buffers\n");
        thread_destroy(worker);   /* never queued; the bus frees the vectors */
        return -ENOMEM;
    }

    /* Register with network interface layer */
    memcpy(net_nif.mac, net_mac, 6);
    net_nif.send = virtio_net_nif_send;
    netif_register(&net_nif);

    net_initialized = 1;
    pci->dev.driver_data = &net_vdev;
    sched_add_thread(worker);
    kprintf("[VIRTIO-NET] Initialized (%d RX buffers, %s)\n", RX_BUF_COUNT, net_vdev.msix ? "MSI-X" : "INTx");
    return 0;
}

/* ADR-0017: the hooks exist; NIC power management comes later. */
static int virtio_net_suspend(Device *dev, int how) {
    (void)how;
    kprintf("[VIRTIO-NET] %s: suspend not supported yet\n", dev->name);
    return -EOPNOTSUPP;
}

static int virtio_net_resume(Device *dev, int how) {
    (void)how;
    kprintf("[VIRTIO-NET] %s: resume not supported yet\n", dev->name);
    return -EOPNOTSUPP;
}

static const PciDeviceId virtio_net_ids[] = {
    PCI_DEVICE(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID),
    {0},
};

static PciDriver virtio_net_driver = {
    .driver = {.name = "virtio-net", .suspend = virtio_net_suspend, .resume = virtio_net_resume},
    .ids = virtio_net_ids,
    .probe = virtio_net_probe,
};

int virtio_net_register(void) {
    return pci_register_driver(&virtio_net_driver);
}

int virtio_net_send(const void *data, uint32_t len) {
    if (!net_initialized) return -1;

    Virtqueue *vq = &net_vdev.queues[TX_QUEUE];
    Device *dev = &net_vdev.pci->dev;
    if (len > PAGE_SIZE - VIRTIO_NET_HDR_SIZE) return -1;

    /* A DMA page for virtio-net header + frame */
    uint64_t phys;
    void *buf = dma_alloc_coherent(dev, PAGE_SIZE, &phys, 0);
    if (buf == NULL) return -1;

    /* Zero the virtio-net header */
    memset(buf, 0, VIRTIO_NET_HDR_SIZE);
    /* Copy ethernet frame after header */
    memcpy((uint8_t *)buf + VIRTIO_NET_HDR_SIZE, data, len);

    /* Build single descriptor (device-readable) */
    uint16_t d = virtq_alloc_desc(vq);
    if (d == VRING_DESC_NONE) {
        dma_free_coherent(dev, PAGE_SIZE, buf, phys);
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
    dma_free_coherent(dev, PAGE_SIZE, buf, phys);

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
