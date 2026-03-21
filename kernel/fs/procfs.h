#ifndef ARCHOS_FS_PROCFS_H
#define ARCHOS_FS_PROCFS_H

#include "fs/vfs.h"

/* Initialize procfs and return the root VfsNode for /proc.
 * All nodes are statically allocated — no kmalloc needed. */
VfsNode *procfs_init(void);

#endif /* ARCHOS_FS_PROCFS_H */
