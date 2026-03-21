#ifndef ARCHOS_FS_DEVFS_H
#define ARCHOS_FS_DEVFS_H

#include "fs/vfs.h"

/* Initialize devfs and return the root VfsNode for /dev.
 * All nodes are statically allocated — no kmalloc needed. */
VfsNode *devfs_init(void);

#endif /* ARCHOS_FS_DEVFS_H */
