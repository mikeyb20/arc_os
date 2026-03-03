#ifndef ARCHOS_FS_RAMFS_H
#define ARCHOS_FS_RAMFS_H

#include "fs/vfs.h"

/* Initialize ramfs and return the root directory VfsNode */
VfsNode *ramfs_init(void);

#endif /* ARCHOS_FS_RAMFS_H */
