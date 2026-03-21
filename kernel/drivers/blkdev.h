#ifndef ARCHOS_DRIVERS_BLKDEV_H
#define ARCHOS_DRIVERS_BLKDEV_H

#include <stdint.h>

#define BLKDEV_MAX 4

/* Block device abstraction — decouples filesystems from specific drivers. */
typedef struct BlockDevice {
    int (*read)(struct BlockDevice *dev, uint64_t sector, uint32_t count, void *buf);
    int (*write)(struct BlockDevice *dev, uint64_t sector, uint32_t count, const void *buf);
    uint64_t (*capacity)(struct BlockDevice *dev);
    void *private_data;
} BlockDevice;

/* Register a block device. Returns slot index (0-3) or -1 on failure. */
int blkdev_register(BlockDevice *dev);

/* Get block device by slot index. Returns NULL if slot is empty. */
BlockDevice *blkdev_get(int index);

#endif /* ARCHOS_DRIVERS_BLKDEV_H */
