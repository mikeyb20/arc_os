#ifndef ARCHOS_FS_DEVFS_H
#define ARCHOS_FS_DEVFS_H

#include "fs/vfs.h"

/* Initialize devfs and return the root VfsNode for /dev.
 * All nodes are statically allocated — no kmalloc needed. */
VfsNode *devfs_init(void);

/* Return 1 if node is a terminal device (/dev/tty), else 0. ioctl uses it. */
int devfs_is_tty(const VfsNode *node);

#endif /* ARCHOS_FS_DEVFS_H */
