#ifndef ARCHOS_FS_FAT32_H
#define ARCHOS_FS_FAT32_H

#include <stdint.h>
#include "fs/vfs.h"

/* FAT32 directory entry attributes */
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F  /* Long filename entry */

/* FAT entry constants */
#define FAT32_FREE            0x00000000
#define FAT32_EOC             0x0FFFFFF8  /* End-of-chain (>= this value) */
#define FAT32_ENTRY_MASK      0x0FFFFFFF  /* Top 4 bits reserved */
#define FAT32_BAD_CLUSTER     0x0FFFFFF7

/* Directory entry markers */
#define FAT32_DIR_FREE        0xE5  /* Deleted entry */
#define FAT32_DIR_END         0x00  /* No more entries */

/* BPB — BIOS Parameter Block (boot sector) */
typedef struct {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;      /* Must be 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;           /* Must be 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32-specific fields */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_serial;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed)) Fat32Bpb;

/* 32-byte FAT32 directory entry */
typedef struct {
    uint8_t  name[11];              /* 8.3 short name */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed)) Fat32DirEntry;

/* Runtime volume context */
typedef struct {
    uint8_t   sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  fat_start;            /* First sector of FAT */
    uint32_t  data_start;           /* First sector of data region */
    uint32_t  root_cluster;
    uint32_t  total_clusters;
    uint32_t *fat;                  /* Cached FAT table */
    uint32_t  fat_sectors;          /* Number of sectors in one FAT */
    int       fat_dirty;            /* Nonzero if FAT modified since last sync */
    VfsNode  *root_node;            /* VFS root for this volume */
} Fat32Volume;

/* Per-node metadata (attached via VfsNode.private_data) */
typedef struct {
    Fat32Volume *vol;
    uint32_t     first_cluster;     /* First cluster of this file/dir */
    uint32_t     dir_cluster;       /* Parent directory's first cluster */
    uint32_t     dir_entry_idx;     /* Index within parent directory */
} Fat32NodeInfo;

/* Mount a FAT32 volume from the VirtIO block device.
 * Returns the root VfsNode, or NULL on failure. */
VfsNode *fat32_mount(void);

/* Flush dirty FAT sectors back to disk. Returns 0 on success. */
int fat32_sync(void);

#endif /* ARCHOS_FS_FAT32_H */
