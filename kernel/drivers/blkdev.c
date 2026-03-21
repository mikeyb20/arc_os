#include "drivers/blkdev.h"
#include <stddef.h>

static BlockDevice *devices[BLKDEV_MAX];
static int device_count;

int blkdev_register(BlockDevice *dev) {
    if (dev == NULL || device_count >= BLKDEV_MAX) return -1;
    devices[device_count] = dev;
    return device_count++;
}

BlockDevice *blkdev_get(int index) {
    if (index < 0 || index >= BLKDEV_MAX) return NULL;
    return devices[index];
}
