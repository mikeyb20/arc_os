#ifndef ARCHOS_FS_EXT2_H
#define ARCHOS_FS_EXT2_H

/* ext2 rev 1 (read M1-13, write M1-15). arc_os's own system and data
 * partitions use it (ADR-0008); 1, 2 and 4 KiB blocks, 128- and 256-byte
 * inodes, the features `filetype`, `sparse_super` and `large_file`.
 * `dir_index`, `ext_attr` and `resize_inode` are ignored safely (htree
 * directories are read linearly, and a directory ext2 changes loses its
 * index flag). Any other incompatible feature refuses the mount; any other
 * read-only-compatible one marks the volume read-only for good. Each is
 * logged by name. Writing needs 4 KiB blocks (see ext2.c). */

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

/* ext2_mount flags */
#define EXT2_MOUNT_RW 0x1   /* read-write, if the volume allows it (4 KiB blocks, no unknown features) */

/* Mount the ext2 volume on dev, which holds `bytes` bytes (a partition or a
 * whole disk); name is for the log ("vda2"). Read-only (SB_RDONLY) unless
 * flags has EXT2_MOUNT_RW and the volume can be written; read-write marks
 * the volume "in use" on the disk before it returns, and a volume found
 * dirty is mounted with a warning. All metadata (superblock, group
 * descriptors, bitmaps, inodes, indirect blocks, directories) goes through a
 * block-device page cache (mm/filemap.h); file data through the files' own
 * cache pages. Returns the superblock (type "ext2"), or NULL with *err:
 * -EINVAL (not ext2, or a geometry or feature it can't mount), -EIO,
 * -ENOMEM. Volumes are never unmounted: ext2_shutdown ends writing. */
SuperBlock *ext2_mount(BlockDevice *dev, uint64_t bytes, const char *name, uint32_t flags, int *err);

/* Before power-off: make the volume read-only (writes get -EROFS), write
 * every dirty page back (data, then metadata), flush the disk, then put the
 * superblock state found at mount back (clean if it was clean). 0, or -errno
 * (the volume stays marked in use). Nothing for a read-only volume. */
int ext2_shutdown(SuperBlock *sb);

/* 1 if the volume refused writing for good (an unknown ro_compat feature or
 * damaged group descriptors). */
int ext2_ro_forced(const SuperBlock *sb);

/* 1 while the volume is mounted read-write (until ext2_shutdown). */
int ext2_writable(const SuperBlock *sb);

#endif /* ARCHOS_FS_EXT2_H */
