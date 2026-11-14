#ifndef ARCHOS_DRIVERS_VIRTIO_BLK_H
#define ARCHOS_DRIVERS_VIRTIO_BLK_H

#include <stdint.h>

/* VirtIO block device vendor/device IDs (legacy) */
#define VIRTIO_BLK_VENDOR_ID  0x1AF4
#define VIRTIO_BLK_DEVICE_ID  0x1001

/* VirtIO block request types */
#define VIRTIO_BLK_T_IN    0  /* Read */
#define VIRTIO_BLK_T_OUT   1  /* Write */
#define VIRTIO_BLK_T_FLUSH 4  /* Write the volatile cache out */

/* VirtIO block status values */
#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2
#define VIRTIO_BLK_STATUS_INIT 0xFF   /* Sentinel — device overwrites on completion */

/* VirtIO block feature bits */
#define VIRTIO_BLK_F_SIZE_MAX   (1 << 1)
#define VIRTIO_BLK_F_SEG_MAX    (1 << 2)
#define VIRTIO_BLK_F_GEOMETRY   (1 << 4)
#define VIRTIO_BLK_F_RO         (1 << 5)
#define VIRTIO_BLK_F_BLK_SIZE   (1 << 6)
#define VIRTIO_BLK_F_FLUSH      (1 << 9)
#define VIRTIO_BLK_F_TOPOLOGY   (1 << 10)

/* Device config offsets (legacy layout) */
#define VIRTIO_BLK_CFG_CAPACITY 0    /* u64, 512-byte sectors */
#define VIRTIO_BLK_CFG_SEG_MAX  12   /* u32 */
#define VIRTIO_BLK_CFG_BLK_SIZE 20   /* u32, logical block size */
#define VIRTIO_BLK_CFG_PHYS_EXP 24   /* u8, physical block = logical << exp */

/* Block request header (sent to device as first descriptor) */
typedef struct {
    uint32_t type;      /* VIRTIO_BLK_T_* */
    uint32_t reserved;
    uint64_t sector;    /* Starting sector (512-byte units, always) */
} __attribute__((packed)) VirtioBlkReqHeader;

/* Register the virtio-blk PCI driver; every virtio-blk device it binds
 * becomes a disk of the block layer (vda, vdb, ...). Returns how many it
 * bound. Nothing else calls this driver: I/O goes through drivers/blkdev.h. */
int virtio_blk_register(void);

#endif /* ARCHOS_DRIVERS_VIRTIO_BLK_H */
