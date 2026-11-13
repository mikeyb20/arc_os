#ifndef ARCHOS_FS_RAMFS_H
#define ARCHOS_FS_RAMFS_H

#include "fs/vfs.h"

/* A new, empty in-memory filesystem (root directory 0755, owned by root).
 * NULL when out of memory. */
SuperBlock *ramfs_create_sb(void);

#endif /* ARCHOS_FS_RAMFS_H */
