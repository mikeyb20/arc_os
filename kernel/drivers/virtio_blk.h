#ifndef ARCHOS_DRIVERS_VIRTIO_BLK_H
#define ARCHOS_DRIVERS_VIRTIO_BLK_H

#include <stdint.h>

/* VirtIO block device vendor/device IDs (legacy) */
#define VIRTIO_BLK_VENDOR_ID  0x1AF4
#define VIRTIO_BLK_DEVICE_ID  0x1001

/* VirtIO block request types */
#define VIRTIO_BLK_T_IN   0  /* Read */
#define VIRTIO_BLK_T_OUT  1  /* Write */

/* VirtIO block status values */
#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

/* VirtIO block feature bits */
#define VIRTIO_BLK_F_SIZE_MAX   (1 << 1)
#define VIRTIO_BLK_F_SEG_MAX    (1 << 2)
#define VIRTIO_BLK_F_GEOMETRY   (1 << 4)
#define VIRTIO_BLK_F_RO         (1 << 5)
#define VIRTIO_BLK_F_BLK_SIZE   (1 << 6)

/* Block request header (sent to device as first descriptor) */
typedef struct {
    uint32_t type;      /* VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT */
    uint32_t reserved;
    uint64_t sector;    /* Starting sector (512-byte units) */
} __attribute__((packed)) VirtioBlkReqHeader;

/* Initialize the VirtIO block device. Returns 0 on success. */
int virtio_blk_init(void);

/* Read sectors from the block device (polling).
 * sector: starting sector number
 * count: number of 512-byte sectors to read
 * buf: destination buffer (must be at least count*512 bytes)
 * Returns 0 on success. */
int virtio_blk_read(uint64_t sector, uint32_t count, void *buf);

/* Write sectors to the block device (stub — returns -1). */
int virtio_blk_write(uint64_t sector, uint32_t count, const void *buf);

/* Get device capacity in 512-byte sectors. */
uint64_t virtio_blk_capacity(void);

#endif /* ARCHOS_DRIVERS_VIRTIO_BLK_H */
