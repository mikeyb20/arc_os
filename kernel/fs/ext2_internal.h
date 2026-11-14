#ifndef ARCHOS_FS_EXT2_INTERNAL_H
#define ARCHOS_FS_EXT2_INTERNAL_H

/* ext2 internals shared by ext2.c (volume, inodes, file pages), ext2_dir.c
 * (directories and names), ext2_map.c (block map, writing, truncate) and
 * ext2_alloc.c (bitmaps, counts, metadata write-back order). Only these four
 * files include it; everything else uses fs/ext2.h. */

#include <stdint.h>
#include <stddef.h>
#include "fs/ext2.h"
#include "fs/vfs.h"
#include "mm/filemap.h"
#include "mm/page.h"
#include "proc/mutex.h"

#define EXT2_MAGIC        0xEF53
#define EXT2_SUPER_OFFSET 1024
#define EXT2_ROOT_INO     2
#define EXT2_NDIR_BLOCKS  12
#define EXT2_IND_BLOCK    12
#define EXT2_N_BLOCKS     15
#define EXT2_FAST_LINK    60   /* bytes of i_block: a fast symlink's target fits below this */

#define EXT2_VALID_FS 1   /* s_state: unmounted cleanly */
#define EXT2_ERROR_FS 2   /* s_state: errors found */

#define EXT2_INDEX_FL 0x1000   /* i_flags: an htree directory (dir_index) */

/* Directory entry file types (`filetype` feature) */
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* --- On-disk layout (little-endian, as the CPU) --- */

/* The superblock's first 136 bytes: all arc_os reads or writes */
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
} Ext2DiskSuper;
_Static_assert(sizeof(Ext2DiskSuper) == 136, "ext2 superblock prefix");
_Static_assert(__builtin_offsetof(Ext2DiskSuper, s_magic) == 56, "s_magic");
_Static_assert(__builtin_offsetof(Ext2DiskSuper, s_state) == 58, "s_state");
_Static_assert(__builtin_offsetof(Ext2DiskSuper, s_feature_compat) == 92, "s_feature_compat");

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
} Ext2DiskGroup;
_Static_assert(sizeof(Ext2DiskGroup) == 32, "ext2 group descriptor");

/* The first 128 bytes of an inode (a 256-byte inode's rest is kept as is,
 * zeros in a new one) */
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;   /* 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;   /* regular files: the top 32 bits of the size (old name i_dir_acl) */
    uint32_t i_faddr;
    uint8_t l_i_frag;
    uint8_t l_i_fsize;
    uint16_t l_i_blocks_hi;
    uint16_t l_i_uid_high;
    uint16_t l_i_gid_high;
    uint32_t l_i_reserved2;
} Ext2DiskInode;
_Static_assert(sizeof(Ext2DiskInode) == 128, "ext2 inode");
_Static_assert(__builtin_offsetof(Ext2DiskInode, i_block) == 40, "i_block");
_Static_assert(__builtin_offsetof(Ext2DiskInode, i_size_high) == 108, "i_size_high");
_Static_assert(__builtin_offsetof(Ext2DiskInode, l_i_uid_high) == 120, "l_i_uid_high");

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;   /* with `filetype`; else the high byte of name_len (always 0 for names <= 255) */
    char name[];
} Ext2DirEntry;
#define EXT2_DIRENT_HEAD 8

/* Bytes an entry with a name of len bytes needs (Linux EXT2_DIR_REC_LEN) */
static inline uint32_t ext2_rec_len(uint32_t len) {
    return (EXT2_DIRENT_HEAD + len + 3) & ~3u;
}

/* --- In memory --- */

/* One block group: where its bitmaps and inode table are, and its counts
 * (the authority while mounted; written to the descriptor at a commit) */
typedef struct Ext2Group {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t used_dirs;
} Ext2Group;

typedef struct Ext2Vol {
    SuperBlock sb;   /* first: an inode's sb is its volume */
    BlockDevice *dev;
    AddressSpace meta;        /* the device's page cache: all metadata, never file data */
    FilemapBlocks blocks;     /* file pages to disk blocks (mm/filemap_block.c) */
    uint32_t block_size;
    uint32_t block_shift;     /* log2(block_size) */
    uint32_t addr_shift;      /* log2(block numbers per indirect block) */
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t groups;
    uint32_t first_data_block;
    uint32_t first_ino;       /* first inode a file may get (11) */
    uint32_t gdt_blocks;      /* blocks of group descriptors after the superblock */
    Ext2Group *grp;           /* per group */
    uint64_t *grp_dirty;      /* bit per group: its counts changed since the last commit */
    uint32_t free_blocks;     /* totals over the groups (under alloc_lock) */
    uint32_t free_inodes;
    uint32_t ro_compat;       /* s_feature_ro_compat as the disk should hold it */
    int super_dirty;          /* the superblock's counts or features changed since the last commit */
    uint16_t mount_state;     /* s_state as found at mount: written back at shutdown (dirty stays dirty) */
    uint32_t generation;      /* the next new inode's i_generation */
    uint32_t dir_rotor;       /* the group the next directory search starts at */
    uint64_t max_bytes;       /* the largest file size */
    int filetype;             /* directory entries carry a type byte */
    int ro_forced;            /* an unknown ro_compat feature: never writable */
    int writable;             /* mounted read-write (M1-15) */
    Mutex alloc_lock;         /* LOCK_LEVEL_FS_ALLOC: bitmaps, counts, every block-map change */
    char name[16];            /* the device, for the log */
} Ext2Vol;

/* Inode.priv */
typedef struct Ext2Inode {
    uint32_t block[EXT2_N_BLOCKS];   /* i_block: the map, or a fast symlink's target */
    uint32_t flags;                  /* i_flags */
    uint32_t fast_link;              /* a symlink whose target is block[]'s bytes */
    uint32_t on_disk;                /* has an inode-table slot and bitmap bit (not a failed create) */
    uint32_t generation;             /* i_generation */
    uint32_t dtime;                  /* i_dtime: set when the inode is freed */
    /* Last-level indirect block used last: (first file block it maps << 32)
     * | its disk block, or 0. One atomic word. */
    uint64_t ind_cache;
    /* Allocation goal for a sequential writer (under alloc_lock): file block
     * next_lblk goes to disk block next_goal (Linux i_next_alloc_*) */
    uint32_t next_lblk;
    uint32_t next_goal;
} Ext2Inode;

static inline Ext2Vol *ext2_vol(const Inode *inode) {
    return (Ext2Vol *)inode->sb;
}

static inline Ext2Inode *ext2_ei(const Inode *inode) {
    return (Ext2Inode *)inode->priv;
}

static inline uint64_t ext2_block_at(const Ext2Vol *v, uint32_t block) {
    return (uint64_t)block << v->block_shift;
}

/* --- ext2.c --- */

/* Log a corrupt structure and return -EIO. */
int ext2_corrupt(const Ext2Vol *v, const char *what, uint64_t n);

/* The cached bytes at disk byte `at` (a range inside one block), read-only:
 * valid until page_put(*pg). NULL with *err = -EIO. */
const uint8_t *ext2_meta_map(Ext2Vol *v, uint64_t at, struct page **pg, int *err);

/* Copy n bytes at disk byte `at` (inside one block) into dst. 0 or -EIO. */
int ext2_meta_read(Ext2Vol *v, uint64_t at, void *dst, size_t n);

/* The same bytes for changing: the page is locked (LOCK_LEVEL_META_PAGE;
 * hold no other metadata page lock, and take alloc_lock before, never
 * under). End with ext2_meta_done. */
uint8_t *ext2_meta_lock(Ext2Vol *v, uint64_t at, struct page **pg, int *err);

/* Release a page from ext2_meta_lock, marking it dirty if changed. */
void ext2_meta_done(struct page *pg, int changed);

/* A block just allocated for metadata (indirect, directory, symlink): its
 * cache page made new and zeroed, locked, without reading the disk. With
 * write_now, ext2_meta_new_done writes it to the disk before it returns, so
 * no pointer to the block can reach the disk ahead of its contents. */
uint8_t *ext2_meta_new(Ext2Vol *v, uint32_t block, struct page **pg, int *err);
int ext2_meta_new_done(Ext2Vol *v, struct page *pg, int write_now);

/* The inode numbered ino: 0 with *out (a reference), or -errno. */
int ext2_iget(Ext2Vol *v, uint32_t ino, Inode **out);

/* The disk byte where inode ino's slot starts */
uint64_t ext2_inode_at(const Ext2Vol *v, uint32_t ino);

/* A new inode of mode in the group of dir (or spread, for directories),
 * owned by uid/gid, nlink 1 (2 for a directory), its table slot zeroed and
 * written: 0 with *out (a reference, hashed) or -errno (-ENOSPC). */
int ext2_new_inode(Inode *dir, uint32_t mode, uint32_t uid, uint32_t gid, Inode **out);

/* Copy the inode's attributes and block map into its inode-table slot
 * (SuperOps.write_inode). The copy is made under the slot's page lock, and
 * every block-map change writes the inode again after it, so the last copy
 * is always the newest map. Nothing on a read-only volume. */
void ext2_write_inode(Inode *inode);

/* File type byte of a directory entry for mode */
uint8_t ext2_mode_to_ft(uint32_t mode);

/* --- ext2_alloc.c (all with alloc_lock held) --- */

/* A free block near goal, marked used: 0 with *out, or -ENOSPC / -EIO. */
int ext2_alloc_block(Ext2Vol *v, uint32_t goal, uint32_t *out);

/* Mark count blocks from first free. meta: they held metadata (indirect,
 * directory, symlink blocks), whose cached pages are dropped so their bytes
 * never reach the disk again. */
void ext2_free_blocks(Ext2Vol *v, uint32_t first, uint32_t count, int meta);

/* A free inode number, for a directory (spread over the groups) or a file
 * (near its parent's group), marked used: 0 with *out, or -ENOSPC / -EIO. */
int ext2_alloc_ino(Ext2Vol *v, uint32_t parent_ino, int dir, uint32_t *out);

/* Mark inode ino free. */
void ext2_free_ino(Ext2Vol *v, uint32_t ino, int dir);

/* Copy the in-memory counts (and features) into the group descriptors and
 * the superblock in the cache, dirtying them. */
void ext2_commit(Ext2Vol *v);

/* The metadata mapping's writepages: commit, then every page but the
 * allocation state (and wait), then the allocation state (bitmaps,
 * descriptors, superblock): see ext2_alloc.c. */
int ext2_meta_writepages(AddressSpace *as);

/* The largest file size of a volume with 2^bits-byte blocks (Linux's
 * ext2_max_size: the block map, and i_blocks counting 512-byte units) */
uint64_t ext2_max_size(uint32_t bits);

/* --- ext2_map.c --- */

/* The disk block holding file block lblk of inode: > 0, 0 for a hole, or
 * -errno. No lock needed. */
int64_t ext2_bmap(Inode *inode, uint32_t lblk);

/* The same, giving a hole a block (and the indirect blocks on the way, each
 * zeroed on the disk before anything points at it): > 0, with *fresh = 1
 * when the data block is new, or -errno (-ENOSPC). newblk != 0: use that
 * block (allocated and written by the caller: a directory or symlink block)
 * instead of allocating one. alloc_lock held, no metadata page locked. */
int64_t ext2_map_alloc(Inode *inode, uint32_t lblk, uint32_t newblk, int *fresh);

/* FilemapBlocks.get_block of regular files */
int64_t ext2_get_block(Inode *inode, uint64_t lblk, int create);

/* Free every block mapping file bytes at or past size (and the indirect
 * blocks left empty). alloc_lock held. */
void ext2_truncate_blocks(Inode *inode, uint64_t size);

/* File operations of regular files and the page-cache operations */
extern const FileOps ext2_file_fops;
extern const AddressSpaceOps ext2_aops;
extern const InodeOps ext2_file_iops;
extern const AddressSpaceOps ext2_meta_aops;

/* fsync of any ext2 file or directory: data, then metadata, then the
 * disk's cache */
int ext2_fsync(File *file);

/* setattr for every ext2 inode (size changes only for regular files) */
int ext2_setattr(Inode *inode, const InodeAttr *attr);

/* --- ext2_dir.c --- */

extern const InodeOps ext2_dir_iops;
extern const FileOps ext2_dir_fops;

/* Clear a changed directory's htree flag (dir_index: the index is not kept
 * up to date, so Linux and e2fsck must read it linearly from now on). */
void ext2_dir_unindex(Inode *dir);

#endif /* ARCHOS_FS_EXT2_INTERNAL_H */
