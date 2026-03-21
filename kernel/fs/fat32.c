#include "fs/fat32.h"
#include "drivers/blkdev.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define SECTOR_SIZE 512
#define CASE_OFFSET ('a' - 'A')

/* --- Node cache: avoids duplicate VfsNodes for the same cluster --- */

#define NODE_CACHE_SIZE 256

static struct {
    uint32_t cluster;
    VfsNode *node;
} node_cache[NODE_CACHE_SIZE];
static int node_cache_count;

static VfsNode *cache_lookup(uint32_t cluster) {
    for (int i = 0; i < node_cache_count; i++) {
        if (node_cache[i].cluster == cluster)
            return node_cache[i].node;
    }
    return NULL;
}

static void cache_insert(uint32_t cluster, VfsNode *node) {
    if (node_cache_count < NODE_CACHE_SIZE) {
        node_cache[node_cache_count].cluster = cluster;
        node_cache[node_cache_count].node = node;
        node_cache_count++;
    }
}

/* --- Forward declarations for VfsOps --- */

static int fat32_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size);
static int fat32_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size);
static VfsNode *fat32_lookup(VfsNode *dir, const char *name);
static VfsNode *fat32_create(VfsNode *dir, const char *name, uint8_t type);
static int fat32_unlink(VfsNode *dir, const char *name);
static int fat32_readdir(VfsNode *dir, VfsDirEntry *entries, uint32_t max);
static void fat32_truncate(VfsNode *node, uint64_t size);

static const VfsOps fat32_file_ops = {
    .read     = fat32_read,
    .write    = fat32_write,
    .truncate = fat32_truncate,
};

static const VfsOps fat32_dir_ops = {
    .read    = fat32_read,
    .lookup  = fat32_lookup,
    .create  = fat32_create,
    .unlink  = fat32_unlink,
    .readdir = fat32_readdir,
};

static uint64_t next_fat_inode = 0x10000;  /* Start high to avoid collision with ramfs */

/* --- Cluster helpers --- */

static uint32_t cluster_to_sector(Fat32Volume *vol, uint32_t cluster) {
    return vol->data_start + (cluster - 2) * vol->sectors_per_cluster;
}

static uint32_t fat32_next_cluster(Fat32Volume *vol, uint32_t cluster) {
    uint32_t entry = vol->fat[cluster] & FAT32_ENTRY_MASK;
    if (entry >= FAT32_EOC) return 0;  /* End of chain */
    if (entry == FAT32_BAD_CLUSTER) return 0;
    return entry;
}

static inline uint32_t fat32_entry_cluster(const Fat32DirEntry *e) {
    return ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
}

static inline void fat32_set_entry_cluster(Fat32DirEntry *e, uint32_t cluster) {
    e->first_cluster_lo = cluster & 0xFFFF;
    e->first_cluster_hi = (cluster >> 16) & 0xFFFF;
}

static int fat32_read_cluster(Fat32Volume *vol, uint32_t cluster, void *buf) {
    uint32_t sector = cluster_to_sector(vol, cluster);
    return vol->dev->read(vol->dev, sector, vol->sectors_per_cluster, buf);
}

static int fat32_write_cluster(Fat32Volume *vol, uint32_t cluster, const void *buf) {
    uint32_t sector = cluster_to_sector(vol, cluster);
    return vol->dev->write(vol->dev, sector, vol->sectors_per_cluster, buf);
}

/* --- FAT manipulation --- */

static uint32_t fat32_alloc_cluster(Fat32Volume *vol) {
    for (uint32_t i = 2; i < vol->total_clusters + 2; i++) {
        if ((vol->fat[i] & FAT32_ENTRY_MASK) == FAT32_FREE) {
            vol->fat[i] = (vol->fat[i] & ~FAT32_ENTRY_MASK) | (FAT32_EOC & FAT32_ENTRY_MASK);
            vol->fat_dirty = 1;
            return i;
        }
    }
    return 0;  /* Disk full */
}

static void fat32_free_chain(Fat32Volume *vol, uint32_t cluster) {
    while (cluster != 0 && cluster < vol->total_clusters + 2) {
        uint32_t next = fat32_next_cluster(vol, cluster);
        vol->fat[cluster] = (vol->fat[cluster] & ~FAT32_ENTRY_MASK) | FAT32_FREE;
        vol->fat_dirty = 1;
        cluster = next;
    }
}

static uint32_t fat32_extend_chain(Fat32Volume *vol, uint32_t last) {
    uint32_t new_cluster = fat32_alloc_cluster(vol);
    if (new_cluster == 0) return 0;
    if (last != 0) {
        vol->fat[last] = (vol->fat[last] & ~FAT32_ENTRY_MASK) | (new_cluster & FAT32_ENTRY_MASK);
        vol->fat_dirty = 1;
    }
    return new_cluster;
}

/* --- 8.3 name conversion --- */

/* Convert FAT32 "HELLO   TXT" to "hello.txt" */
static void fat32_name_to_str(const Fat32DirEntry *entry, char *out) {
    int pos = 0;

    /* Base name (first 8 chars, trim trailing spaces) */
    int base_len = 8;
    while (base_len > 0 && entry->name[base_len - 1] == ' ') base_len--;
    for (int i = 0; i < base_len; i++) {
        char c = entry->name[i];
        if (c >= 'A' && c <= 'Z') c += CASE_OFFSET;
        out[pos++] = c;
    }

    /* Extension (last 3 chars, trim trailing spaces) */
    int ext_len = 3;
    while (ext_len > 0 && entry->name[8 + ext_len - 1] == ' ') ext_len--;
    if (ext_len > 0) {
        out[pos++] = '.';
        for (int i = 0; i < ext_len; i++) {
            char c = entry->name[8 + i];
            if (c >= 'A' && c <= 'Z') c += CASE_OFFSET;
            out[pos++] = c;
        }
    }

    out[pos] = '\0';
}

/* Convert "hello.txt" to "HELLO   TXT". Returns -1 if name doesn't fit 8.3. */
static int fat32_str_to_name(const char *str, uint8_t out[11]) {
    memset(out, ' ', 11);

    /* Find dot */
    const char *dot = strchr(str, '.');
    int base_len = dot ? (int)(dot - str) : (int)strlen(str);
    int ext_len = dot ? (int)strlen(dot + 1) : 0;

    if (base_len == 0 || base_len > 8 || ext_len > 3) return -1;

    /* Copy base, uppercase */
    for (int i = 0; i < base_len; i++) {
        char c = str[i];
        if (c >= 'a' && c <= 'z') c -= CASE_OFFSET;
        out[i] = (uint8_t)c;
    }

    /* Copy extension, uppercase */
    if (dot) {
        for (int i = 0; i < ext_len; i++) {
            char c = dot[1 + i];
            if (c >= 'a' && c <= 'z') c -= CASE_OFFSET;
            out[8 + i] = (uint8_t)c;
        }
    }

    return 0;
}

/* Case-insensitive 8.3 compare: convert str to 8.3, compare with entry */
static int fat32_name_match(const Fat32DirEntry *entry, const char *name) {
    uint8_t fat_name[11];
    if (fat32_str_to_name(name, fat_name) != 0) {
        /* Name doesn't fit 8.3 — try comparing the human-readable form */
        char entry_str[13];
        fat32_name_to_str(entry, entry_str);
        /* Case-insensitive compare */
        const char *a = entry_str;
        const char *b = name;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += CASE_OFFSET;
            if (cb >= 'A' && cb <= 'Z') cb += CASE_OFFSET;
            if (ca != cb) return 0;
            a++; b++;
        }
        return (*a == '\0' && *b == '\0');
    }
    return memcmp(entry->name, fat_name, 11) == 0;
}

/* --- Node allocation --- */

static VfsNode *fat32_alloc_node(Fat32Volume *vol, uint8_t type,
                                  uint32_t first_cluster,
                                  uint32_t dir_cluster, uint32_t entry_idx) {
    /* Check cache first */
    if (first_cluster != 0) {
        VfsNode *cached = cache_lookup(first_cluster);
        if (cached) return cached;
    }

    Fat32NodeInfo *info = kmalloc(sizeof(Fat32NodeInfo), GFP_ZERO);
    if (!info) return NULL;

    VfsNode *node = kmalloc(sizeof(VfsNode), GFP_ZERO);
    if (!node) {
        kfree(info);
        return NULL;
    }

    info->vol = vol;
    info->first_cluster = first_cluster;
    info->dir_cluster = dir_cluster;
    info->dir_entry_idx = entry_idx;

    node->inode_num = next_fat_inode++;
    node->type = type;
    node->size = 0;
    node->mode = (type == VFS_DIRECTORY) ? 0755 : 0644;
    node->ops = (type == VFS_DIRECTORY) ? &fat32_dir_ops : &fat32_file_ops;
    node->private_data = info;

    if (first_cluster != 0) {
        cache_insert(first_cluster, node);
    }

    return node;
}

static Fat32NodeInfo *to_fat32(VfsNode *node) {
    return (Fat32NodeInfo *)node->private_data;
}

/* --- Cluster chain read/write --- */

static int fat32_read_chain(Fat32Volume *vol, uint32_t start_cluster,
                             uint32_t offset, void *buf, uint32_t size) {
    if (start_cluster == 0) return 0;

    uint32_t bpc = vol->bytes_per_cluster;
    uint8_t *cluster_buf = kmalloc(bpc, 0);
    if (!cluster_buf) return -ENOMEM;

    uint32_t bytes_read = 0;
    uint32_t cluster = start_cluster;
    uint32_t cluster_offset = 0;  /* Byte offset from start of chain */

    /* Skip clusters until we reach the offset */
    while (cluster != 0 && cluster_offset + bpc <= offset) {
        cluster_offset += bpc;
        cluster = fat32_next_cluster(vol, cluster);
    }

    /* Read data */
    while (cluster != 0 && bytes_read < size) {
        if (fat32_read_cluster(vol, cluster, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -EIO;
        }

        uint32_t in_cluster_off = 0;
        if (cluster_offset < offset) {
            in_cluster_off = offset - cluster_offset;
        }

        uint32_t avail = bpc - in_cluster_off;
        uint32_t to_copy = size - bytes_read;
        if (to_copy > avail) to_copy = avail;

        memcpy((uint8_t *)buf + bytes_read, cluster_buf + in_cluster_off, to_copy);
        bytes_read += to_copy;

        cluster_offset += bpc;
        cluster = fat32_next_cluster(vol, cluster);
    }

    kfree(cluster_buf);
    return (int)bytes_read;
}

/* Zero-fill and write a newly allocated cluster. Returns 0 on success, -EIO on failure. */
static int fat32_init_cluster(Fat32Volume *vol, uint32_t cluster, uint8_t *cluster_buf) {
    memset(cluster_buf, 0, vol->bytes_per_cluster);
    return fat32_write_cluster(vol, cluster, cluster_buf) != 0 ? -EIO : 0;
}

/* Walk/extend chain and write data to disk. Updates *first_cluster_ptr if file was empty. */
static int fat32_write_chain(Fat32Volume *vol, uint32_t *first_cluster_ptr,
                              uint32_t offset, const void *buf, uint32_t size) {
    uint32_t bpc = vol->bytes_per_cluster;
    uint8_t *cluster_buf = kmalloc(bpc, 0);
    if (!cluster_buf) return -ENOMEM;

    uint32_t bytes_written = 0;
    uint32_t cluster = *first_cluster_ptr;
    uint32_t prev_cluster = 0;
    uint32_t cluster_offset = 0;

    /* If file has no clusters yet, allocate the first one */
    if (cluster == 0) {
        cluster = fat32_alloc_cluster(vol);
        if (cluster == 0) { kfree(cluster_buf); return -ENOSPC; }
        *first_cluster_ptr = cluster;
        fat32_init_cluster(vol, cluster, cluster_buf);
    }

    /* Skip clusters until we reach the offset */
    while (cluster != 0 && cluster_offset + bpc <= offset) {
        prev_cluster = cluster;
        cluster_offset += bpc;
        cluster = fat32_next_cluster(vol, cluster);
        /* Need to extend chain to reach offset */
        if (cluster == 0 && cluster_offset + bpc <= offset) {
            cluster = fat32_extend_chain(vol, prev_cluster);
            if (cluster == 0) { kfree(cluster_buf); return -ENOSPC; }
            fat32_init_cluster(vol, cluster, cluster_buf);
        }
    }

    /* Extend if we still need a cluster at the offset position */
    if (cluster == 0) {
        cluster = fat32_extend_chain(vol, prev_cluster);
        if (cluster == 0) { kfree(cluster_buf); return -ENOSPC; }
        fat32_init_cluster(vol, cluster, cluster_buf);
    }

    /* Write data */
    while (cluster != 0 && bytes_written < size) {
        /* Read existing cluster data for partial writes */
        if (fat32_read_cluster(vol, cluster, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -EIO;
        }

        uint32_t in_cluster_off = 0;
        if (cluster_offset < offset) {
            in_cluster_off = offset - cluster_offset;
        }

        uint32_t avail = bpc - in_cluster_off;
        uint32_t to_copy = size - bytes_written;
        if (to_copy > avail) to_copy = avail;

        memcpy(cluster_buf + in_cluster_off, (const uint8_t *)buf + bytes_written, to_copy);

        if (fat32_write_cluster(vol, cluster, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -EIO;
        }

        bytes_written += to_copy;
        cluster_offset += bpc;
        prev_cluster = cluster;
        cluster = fat32_next_cluster(vol, cluster);

        /* Extend chain if more data to write */
        if (cluster == 0 && bytes_written < size) {
            cluster = fat32_extend_chain(vol, prev_cluster);
            if (cluster == 0) { kfree(cluster_buf); return -ENOSPC; }
            fat32_init_cluster(vol, cluster, cluster_buf);
        }
    }

    kfree(cluster_buf);
    return (int)bytes_written;
}

/* --- Directory entry helpers --- */

/* Read the idx-th 32-byte dir entry from a directory's cluster chain */
static int fat32_read_dir_entry(Fat32Volume *vol, uint32_t dir_cluster,
                                 uint32_t idx, Fat32DirEntry *out) {
    uint32_t byte_offset = idx * sizeof(Fat32DirEntry);
    return fat32_read_chain(vol, dir_cluster, byte_offset, out, sizeof(Fat32DirEntry));
}

/* Write a dir entry back at the given index */
static int fat32_write_dir_entry(Fat32Volume *vol, uint32_t dir_cluster,
                                  uint32_t idx, const Fat32DirEntry *entry) {
    uint32_t byte_offset = idx * sizeof(Fat32DirEntry);
    return fat32_write_chain(vol, &dir_cluster, byte_offset, entry, sizeof(Fat32DirEntry));
}

/* --- Directory entry walker --- */

#define DIR_WALK_CONTINUE  0
#define DIR_WALK_STOP      1

typedef int (*DirEntryVisitor)(Fat32DirEntry *entry, uint32_t idx, void *ctx);

/* Walk every 32-byte entry in a directory's cluster chain.
 * Returns DIR_WALK_STOP if visitor stopped, 0 if all entries visited,
 * or negative errno on infrastructure error. */
static int fat32_walk_dir(Fat32Volume *vol, uint32_t dir_cluster,
                           DirEntryVisitor visitor, void *ctx) {
    uint32_t epc = vol->bytes_per_cluster / sizeof(Fat32DirEntry);
    uint32_t cluster = dir_cluster;
    uint32_t base_idx = 0;

    while (cluster != 0) {
        uint8_t *buf = kmalloc(vol->bytes_per_cluster, 0);
        if (!buf) return -ENOMEM;
        if (fat32_read_cluster(vol, cluster, buf) != 0) {
            kfree(buf);
            return -EIO;
        }

        Fat32DirEntry *entries = (Fat32DirEntry *)buf;
        for (uint32_t i = 0; i < epc; i++) {
            int rc = visitor(&entries[i], base_idx + i, ctx);
            if (rc != DIR_WALK_CONTINUE) {
                kfree(buf);
                return rc;
            }
        }

        kfree(buf);
        base_idx += epc;
        cluster = fat32_next_cluster(vol, cluster);
    }

    return 0;
}

/* Filter special directory entries.
 * Returns: -1 = end of directory, 1 = skip (free/LFN/volume), 0 = normal entry */
static int fat32_entry_filter(const Fat32DirEntry *e) {
    if (e->name[0] == FAT32_DIR_END) return -1;
    if (e->name[0] == FAT32_DIR_FREE) return 1;
    if (e->attr == FAT32_ATTR_LFN) return 1;
    if (e->attr & FAT32_ATTR_VOLUME_ID) return 1;
    return 0;
}

typedef struct {
    int slot;
} FreeSlotCtx;

static int free_slot_visitor(Fat32DirEntry *e, uint32_t idx, void *arg) {
    if (e->name[0] == FAT32_DIR_END || e->name[0] == FAT32_DIR_FREE) {
        FreeSlotCtx *ctx = (FreeSlotCtx *)arg;
        ctx->slot = (int)idx;
        return DIR_WALK_STOP;
    }
    return DIR_WALK_CONTINUE;
}

/* Find a free slot (0x00 or 0xE5) in directory, or extend it. Returns index or -1. */
static int fat32_find_free_dir_slot(Fat32Volume *vol, uint32_t dir_cluster) {
    FreeSlotCtx ctx = { .slot = -1 };
    int rc = fat32_walk_dir(vol, dir_cluster, free_slot_visitor, &ctx);
    if (rc == DIR_WALK_STOP) return ctx.slot;
    if (rc < 0) return -1;

    /* No free slot — extend the directory */
    uint32_t epc = vol->bytes_per_cluster / sizeof(Fat32DirEntry);
    uint32_t cluster = dir_cluster;
    uint32_t prev = 0;
    uint32_t count = 0;
    while (cluster != 0) {
        prev = cluster;
        cluster = fat32_next_cluster(vol, cluster);
        count++;
    }
    cluster = fat32_extend_chain(vol, prev);
    if (cluster == 0) return -1;
    uint8_t *zero_buf = kmalloc(vol->bytes_per_cluster, GFP_ZERO);
    if (!zero_buf) return -1;
    fat32_write_cluster(vol, cluster, zero_buf);
    kfree(zero_buf);
    return (int)(count * epc);
}

/* --- VfsOps implementations --- */

static int fat32_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    Fat32NodeInfo *info = to_fat32(node);
    if (node->type == VFS_FILE) {
        if (offset >= node->size) return 0;
        uint32_t avail = (uint32_t)(node->size - offset);
        if (size > avail) size = avail;
    }
    return fat32_read_chain(info->vol, info->first_cluster, offset, buf, size);
}

static int fat32_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size) {
    Fat32NodeInfo *info = to_fat32(node);
    if (node->type != VFS_FILE) return -EISDIR;

    int written = fat32_write_chain(info->vol, &info->first_cluster, offset, buf, size);
    if (written <= 0) return written;

    /* Update file size */
    uint32_t end = offset + (uint32_t)written;
    if (end > node->size) {
        node->size = end;
    }

    /* Update directory entry on disk */
    Fat32DirEntry dentry;
    if (fat32_read_dir_entry(info->vol, info->dir_cluster, info->dir_entry_idx, &dentry) > 0) {
        dentry.file_size = (uint32_t)node->size;
        fat32_set_entry_cluster(&dentry, info->first_cluster);
        fat32_write_dir_entry(info->vol, info->dir_cluster, info->dir_entry_idx, &dentry);
    }

    fat32_sync();
    return written;
}

typedef struct {
    const char *name;
    Fat32Volume *vol;
    uint32_t dir_cluster;
    VfsNode *result;
} LookupCtx;

static int lookup_visitor(Fat32DirEntry *e, uint32_t idx, void *arg) {
    int f = fat32_entry_filter(e);
    if (f < 0) return DIR_WALK_STOP;
    if (f > 0) return DIR_WALK_CONTINUE;
    if (e->name[0] == '.' && e->name[1] == ' ') return DIR_WALK_CONTINUE;
    if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == ' ') return DIR_WALK_CONTINUE;

    LookupCtx *ctx = (LookupCtx *)arg;
    if (fat32_name_match(e, ctx->name)) {
        uint32_t fc = fat32_entry_cluster(e);
        uint8_t type = (e->attr & FAT32_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;
        ctx->result = fat32_alloc_node(ctx->vol, type, fc, ctx->dir_cluster, idx);
        if (ctx->result) ctx->result->size = e->file_size;
        return DIR_WALK_STOP;
    }
    return DIR_WALK_CONTINUE;
}

static VfsNode *fat32_lookup(VfsNode *dir, const char *name) {
    Fat32NodeInfo *dir_info = to_fat32(dir);
    LookupCtx ctx = { .name = name, .vol = dir_info->vol,
                       .dir_cluster = dir_info->first_cluster, .result = NULL };
    fat32_walk_dir(dir_info->vol, dir_info->first_cluster, lookup_visitor, &ctx);
    return ctx.result;
}

static VfsNode *fat32_create(VfsNode *dir, const char *name, uint8_t type) {
    Fat32NodeInfo *dir_info = to_fat32(dir);
    Fat32Volume *vol = dir_info->vol;

    /* Check duplicate */
    if (fat32_lookup(dir, name) != NULL) return NULL;

    /* Build 8.3 name */
    uint8_t fat_name[11];
    if (fat32_str_to_name(name, fat_name) != 0) return NULL;

    /* Allocate cluster for directories */
    uint32_t new_cluster = 0;
    if (type == VFS_DIRECTORY) {
        new_cluster = fat32_alloc_cluster(vol);
        if (new_cluster == 0) return NULL;
        /* Zero and init . and .. entries */
        uint8_t *cbuf = kmalloc(vol->bytes_per_cluster, GFP_ZERO);
        if (!cbuf) return NULL;
        Fat32DirEntry *dot_entries = (Fat32DirEntry *)cbuf;
        /* . entry */
        memcpy(dot_entries[0].name, ".          ", 11);
        dot_entries[0].attr = FAT32_ATTR_DIRECTORY;
        fat32_set_entry_cluster(&dot_entries[0], new_cluster);
        /* .. entry */
        memcpy(dot_entries[1].name, "..         ", 11);
        dot_entries[1].attr = FAT32_ATTR_DIRECTORY;
        fat32_set_entry_cluster(&dot_entries[1], dir_info->first_cluster);
        fat32_write_cluster(vol, new_cluster, cbuf);
        kfree(cbuf);
    }

    /* Find free dir entry slot */
    int slot = fat32_find_free_dir_slot(vol, dir_info->first_cluster);
    if (slot < 0) return NULL;

    /* Build directory entry */
    Fat32DirEntry dentry;
    memset(&dentry, 0, sizeof(dentry));
    memcpy(dentry.name, fat_name, 11);
    dentry.attr = (type == VFS_DIRECTORY) ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;
    fat32_set_entry_cluster(&dentry, new_cluster);
    dentry.file_size = 0;

    fat32_write_dir_entry(vol, dir_info->first_cluster, (uint32_t)slot, &dentry);
    fat32_sync();

    VfsNode *node = fat32_alloc_node(vol, type, new_cluster,
                                      dir_info->first_cluster, (uint32_t)slot);
    return node;
}

typedef struct {
    const char *name;
    Fat32Volume *vol;
    uint32_t dir_cluster;
    int result;
} UnlinkCtx;

static int unlink_visitor(Fat32DirEntry *e, uint32_t idx, void *arg) {
    int f = fat32_entry_filter(e);
    if (f < 0) return DIR_WALK_STOP;
    if (f > 0) return DIR_WALK_CONTINUE;

    UnlinkCtx *ctx = (UnlinkCtx *)arg;
    if (fat32_name_match(e, ctx->name)) {
        uint32_t fc = fat32_entry_cluster(e);
        if (fc != 0) fat32_free_chain(ctx->vol, fc);
        e->name[0] = FAT32_DIR_FREE;
        fat32_write_dir_entry(ctx->vol, ctx->dir_cluster, idx, e);
        fat32_sync();
        ctx->result = VFS_OK;
        return DIR_WALK_STOP;
    }
    return DIR_WALK_CONTINUE;
}

static int fat32_unlink(VfsNode *dir, const char *name) {
    Fat32NodeInfo *dir_info = to_fat32(dir);
    UnlinkCtx ctx = { .name = name, .vol = dir_info->vol,
                       .dir_cluster = dir_info->first_cluster, .result = -ENOENT };
    int rc = fat32_walk_dir(dir_info->vol, dir_info->first_cluster, unlink_visitor, &ctx);
    if (rc < 0) return rc;
    return ctx.result;
}

typedef struct {
    VfsDirEntry *out;
    uint32_t max;
    uint32_t count;
} ReaddirCtx;

static int readdir_visitor(Fat32DirEntry *e, uint32_t idx, void *arg) {
    (void)idx;
    int f = fat32_entry_filter(e);
    if (f < 0) return DIR_WALK_STOP;
    if (f > 0) return DIR_WALK_CONTINUE;
    if (e->name[0] == '.') return DIR_WALK_CONTINUE;

    ReaddirCtx *ctx = (ReaddirCtx *)arg;
    fat32_name_to_str(e, ctx->out[ctx->count].name);
    ctx->out[ctx->count].inode_num = fat32_entry_cluster(e);
    ctx->out[ctx->count].type = (e->attr & FAT32_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;
    ctx->count++;
    if (ctx->count >= ctx->max) return DIR_WALK_STOP;
    return DIR_WALK_CONTINUE;
}

static int fat32_readdir(VfsNode *dir, VfsDirEntry *out, uint32_t max) {
    Fat32NodeInfo *dir_info = to_fat32(dir);
    ReaddirCtx ctx = { .out = out, .max = max, .count = 0 };
    fat32_walk_dir(dir_info->vol, dir_info->first_cluster, readdir_visitor, &ctx);
    return (int)ctx.count;
}

static void fat32_truncate(VfsNode *node, uint64_t size) {
    Fat32NodeInfo *info = to_fat32(node);
    if (node->type != VFS_FILE) return;

    if (size == 0) {
        if (info->first_cluster != 0) {
            fat32_free_chain(info->vol, info->first_cluster);
            info->first_cluster = 0;
        }
        node->size = 0;
    } else if (size < node->size) {
        /* Walk chain to the cluster containing 'size', free the rest */
        uint32_t bpc = info->vol->bytes_per_cluster;
        uint32_t keep_clusters = ((uint32_t)size + bpc - 1) / bpc;
        uint32_t cluster = info->first_cluster;
        for (uint32_t i = 1; i < keep_clusters && cluster != 0; i++) {
            cluster = fat32_next_cluster(info->vol, cluster);
        }
        if (cluster != 0) {
            uint32_t next = fat32_next_cluster(info->vol, cluster);
            /* Mark current as EOC */
            info->vol->fat[cluster] = (info->vol->fat[cluster] & ~FAT32_ENTRY_MASK) |
                                       (FAT32_EOC & FAT32_ENTRY_MASK);
            info->vol->fat_dirty = 1;
            /* Free the rest */
            if (next != 0) fat32_free_chain(info->vol, next);
        }
        node->size = size;
    }

    /* Update dir entry */
    Fat32DirEntry dentry;
    if (fat32_read_dir_entry(info->vol, info->dir_cluster, info->dir_entry_idx, &dentry) > 0) {
        dentry.file_size = (uint32_t)node->size;
        fat32_set_entry_cluster(&dentry, info->first_cluster);
        fat32_write_dir_entry(info->vol, info->dir_cluster, info->dir_entry_idx, &dentry);
    }
    fat32_sync();
}

/* --- Sync --- */

int fat32_sync(void) {
    /* Find the volume from the root node cache — simple approach */
    if (node_cache_count == 0) return -1;
    Fat32NodeInfo *info = (Fat32NodeInfo *)node_cache[0].node->private_data;
    Fat32Volume *vol = info->vol;

    if (!vol->fat_dirty) return 0;

    /* Write FAT sectors back to disk */
    uint8_t sector_buf[SECTOR_SIZE];
    for (uint32_t s = 0; s < vol->fat_sectors; s++) {
        memcpy(sector_buf, (uint8_t *)vol->fat + s * SECTOR_SIZE, SECTOR_SIZE);
        if (vol->dev->write(vol->dev, vol->fat_start + s, 1, sector_buf) != 0) {
            kprintf("[FAT32] Failed to sync FAT sector %u\n", s);
            return -EIO;
        }
    }

    vol->fat_dirty = 0;
    return 0;
}

/* --- Mount --- */

VfsNode *fat32_mount(BlockDevice *dev) {
    if (dev == NULL) return NULL;

    /* Read boot sector */
    uint8_t sector_buf[SECTOR_SIZE];
    if (dev->read(dev, 0, 1, sector_buf) != 0) {
        kprintf("[FAT32] Failed to read boot sector\n");
        return NULL;
    }

    Fat32Bpb *bpb = (Fat32Bpb *)sector_buf;

    /* Validate FAT32 */
    if (bpb->bytes_per_sector != 512) {
        kprintf("[FAT32] Unsupported sector size: %u\n", bpb->bytes_per_sector);
        return NULL;
    }
    if (bpb->root_entry_count != 0) {
        kprintf("[FAT32] Not FAT32 (root_entry_count=%u)\n", bpb->root_entry_count);
        return NULL;
    }
    if (bpb->fat_size_32 == 0) {
        kprintf("[FAT32] fat_size_32 is zero\n");
        return NULL;
    }
    if (bpb->sectors_per_cluster == 0) {
        kprintf("[FAT32] sectors_per_cluster is zero\n");
        return NULL;
    }

    /* Allocate volume context */
    Fat32Volume *vol = kmalloc(sizeof(Fat32Volume), GFP_ZERO);
    if (!vol) return NULL;

    vol->dev = dev;
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->bytes_per_cluster = (uint32_t)bpb->sectors_per_cluster * SECTOR_SIZE;
    vol->fat_start = bpb->reserved_sectors;
    vol->fat_sectors = bpb->fat_size_32;
    vol->root_cluster = bpb->root_cluster;

    /* Data region starts after reserved + all FATs */
    vol->data_start = bpb->reserved_sectors + bpb->num_fats * bpb->fat_size_32;

    /* Total clusters */
    uint32_t total_sectors = bpb->total_sectors_32;
    uint32_t data_sectors = total_sectors - vol->data_start;
    vol->total_clusters = data_sectors / bpb->sectors_per_cluster;

    kprintf("[FAT32] BPB: spc=%u fat_start=%u data_start=%u root_cluster=%u total_clusters=%u\n",
            vol->sectors_per_cluster, vol->fat_start, vol->data_start,
            vol->root_cluster, vol->total_clusters);

    /* Load entire FAT into memory */
    uint32_t fat_bytes = vol->fat_sectors * SECTOR_SIZE;
    vol->fat = kmalloc(fat_bytes, 0);
    if (!vol->fat) {
        kprintf("[FAT32] Failed to allocate FAT cache (%u bytes)\n", fat_bytes);
        kfree(vol);
        return NULL;
    }

    /* Read FAT sectors one at a time */
    for (uint32_t s = 0; s < vol->fat_sectors; s++) {
        if (dev->read(dev, vol->fat_start + s, 1,
                      (uint8_t *)vol->fat + s * SECTOR_SIZE) != 0) {
            kprintf("[FAT32] Failed to read FAT sector %u\n", s);
            kfree(vol->fat);
            kfree(vol);
            return NULL;
        }
    }

    vol->fat_dirty = 0;

    /* Reset node cache */
    node_cache_count = 0;

    /* Create root VfsNode */
    VfsNode *root = fat32_alloc_node(vol, VFS_DIRECTORY, vol->root_cluster, 0, 0);
    if (!root) {
        kfree(vol->fat);
        kfree(vol);
        return NULL;
    }
    vol->root_node = root;

    kprintf("[FAT32] Volume mounted, FAT cached (%u bytes)\n", fat_bytes);
    return root;
}
