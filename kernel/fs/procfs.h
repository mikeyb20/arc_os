#ifndef ARCHOS_FS_PROCFS_H
#define ARCHOS_FS_PROCFS_H

#include "fs/vfs.h"

/* Set up /proc (fixed files static, per-process entries made per lookup).
 * Call once after vfs_init; returns its superblock. */
SuperBlock *procfs_init(void);

#endif /* ARCHOS_FS_PROCFS_H */
