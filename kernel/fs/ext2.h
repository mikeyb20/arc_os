#ifndef ARCHOS_FS_EXT2_H
#define ARCHOS_FS_EXT2_H

/* ext2 rev 1, read-only (M1-13; writing is M1-15). arc_os's own system and
 * data partitions use it (ADR-0008); 1, 2 and 4 KiB blocks, 128- and
 * 256-byte inodes, the features `filetype`, `sparse_super` and `large_file`.
 * `dir_index`, `ext_attr` and `resize_inode` are ignored safely (htree
 * directories read linearly). Any other incompatible feature refuses the
 * mount; any other read-only-compatible one marks the volume read-only for
 * good (it is read-only anyway until M1-15). Each is logged by name. */

#include <stdint.h>
#include "fs/vfs.h"

typedef struct BlockDevice BlockDevice;

/* Feature bits (Linux include/linux/ext2_fs.h names) */
#define EXT2_FEATURE_COMPAT_DIR_PREALLOC  0x0001
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL   0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR      0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_INODE  0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX     0x0020

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002

/* Mount the ext2 volume on dev, which holds `bytes` bytes (a partition or a
 * whole disk); name is for the log ("vda2"). Read-only (SB_RDONLY). All
 * metadata (superblock, group descriptors, inodes, indirect blocks,
 * directories) is read through a block-device page cache (mm/filemap.h);
 * file data straight into the files' own cache pages. Returns the superblock
 * (type "ext2"), or NULL with *err: -EINVAL (not ext2, or a geometry or
 * feature it can't mount), -EIO, -ENOMEM. Volumes are never unmounted. */
SuperBlock *ext2_mount(BlockDevice *dev, uint64_t bytes, const char *name, int *err);

/* 1 if the volume refused writing for good (an unknown ro_compat feature):
 * M1-15's write support must keep it read-only. */
int ext2_ro_forced(const SuperBlock *sb);

#endif /* ARCHOS_FS_EXT2_H */
