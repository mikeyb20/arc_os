/* arc_os — FAT32, read-only (M1-11).
 *
 * The ESP format (ADR-0008); arc_os keeps its own files on ext2, so FAT32
 * needs lookup, read and getdents only. 8.3 names, no long file names.
 *
 * Inodes: a directory entry's position is its inode number,
 * (directory's first cluster << 16) | entry index, so files without clusters
 * get one too; the root is FAT32_ROOT_INO. Inodes live in the inode cache and
 * the dentry cache (M1-11 fixed the old node cache, which never freed one).
 *
 * File data lives in the page cache (M1-12): fat32_readpage reads one page's
 * clusters straight into it; read() and mmap() are the cache's.
 *
 * Locking: the FAT is read once at mount and never changes. vol->io_lock
 * (LOCK_LEVEL_FS_IO, inside the page lock readpage runs under) serializes the
 * block device and each inode's chain cursor; user copies and dir_emit happen
 * after it is released. */

#include "fs/fat32.h"
#include "drivers/blkdev.h"
#include "mm/kmalloc.h"
#include "mm/filemap.h"
#include "proc/mutex.h"
#include "proc/lockdep.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

#define SECTOR_SIZE 512
#define CASE_OFFSET ('a' - 'A')
#define FAT32_ROOT_INO 1

typedef struct Fat32Volume {
    SuperBlock sb;   /* first: sb->priv is not needed to find the volume */
    BlockDevice *dev;
    uint8_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t fat_start;      /* first sector of the FAT */
    uint32_t data_start;     /* first sector of the data region */
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint32_t *fat;           /* the whole first FAT, read at mount */
    uint32_t fat_sectors;
    Mutex io_lock;
} Fat32Volume;

/* Inode.priv */
typedef struct Fat32Inode {
    uint32_t first_cluster;
    uint64_t parent_ino;     /* directories: ".." for getdents */
    Inode *parent;           /* directories: ".." for the walk (a reference; NULL for the root) */
    /* Chain cursor: cluster number cur_cluster is the cur_index-th of the
     * chain (io_lock). Sequential reads walk the FAT once, not once per call. */
    uint32_t cur_index;
    uint32_t cur_cluster;
} Fat32Inode;

static const InodeOps fat32_dir_iops;
static const FileOps fat32_dir_fops;
static const FileOps fat32_file_fops;
static const AddressSpaceOps fat32_aops;

static Fat32Volume *vol_of(const Inode *inode) {
    return (Fat32Volume *)inode->sb;
}

static Fat32Inode *fi_of(const Inode *inode) {
    return (Fat32Inode *)inode->priv;
}

/* --- Clusters --- */

static int cluster_valid(const Fat32Volume *vol, uint32_t cluster) {
    return cluster >= 2 && cluster < vol->total_clusters + 2;
}

/* Next cluster of the chain, or 0 at its end (or at a bad entry) */
static uint32_t fat32_next_cluster(const Fat32Volume *vol, uint32_t cluster) {
    uint32_t entry = vol->fat[cluster] & FAT32_ENTRY_MASK;
    return cluster_valid(vol, entry) ? entry : 0;
}

static inline uint32_t fat32_entry_cluster(const Fat32DirEntry *e) {
    return ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
}

/* Read one cluster into buf. io_lock held. */
static int fat32_read_cluster(Fat32Volume *vol, uint32_t cluster, void *buf) {
    uint64_t sector = vol->data_start + (uint64_t)(cluster - 2) * vol->sectors_per_cluster;
    return vol->dev->read(vol->dev, sector, vol->sectors_per_cluster, buf) != 0 ? -EIO : 0;
}

/* The index-th cluster of fi's chain, or 0 past its end. io_lock held.
 * Moves the cursor; a chain longer than the volume is cut off (a FAT loop). */
static uint32_t chain_cluster(Fat32Volume *vol, Fat32Inode *fi, uint32_t index) {
    if (fi->cur_cluster == 0 || index < fi->cur_index) {
        fi->cur_index = 0;
        fi->cur_cluster = cluster_valid(vol, fi->first_cluster) ? fi->first_cluster : 0;
    }
    while (fi->cur_cluster != 0 && fi->cur_index < index) {
        if (fi->cur_index >= vol->total_clusters) return 0;
        fi->cur_cluster = fat32_next_cluster(vol, fi->cur_cluster);
        fi->cur_index++;
    }
    return fi->cur_index == index ? fi->cur_cluster : 0;
}

/* Read the index-th cluster of inode's chain into buf: 1, 0 past the end,
 * or -EIO. */
static int read_chain_cluster(Inode *inode, uint32_t index, void *buf) {
    Fat32Volume *vol = vol_of(inode);
    mutex_lock(&vol->io_lock);
    uint32_t c = chain_cluster(vol, fi_of(inode), index);
    int r = c == 0 ? 0 : fat32_read_cluster(vol, c, buf);
    mutex_unlock(&vol->io_lock);
    return c == 0 ? 0 : (r == 0 ? 1 : r);
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

/* --- Inodes --- */

static uint64_t entry_ino(uint32_t dir_cluster, uint32_t idx) {
    return ((uint64_t)dir_cluster << 16) | idx;
}

/* Linux vfat defaults (umask 022): everything rwxr-xr-x, write bits off for
 * read-only entries. */
static uint32_t entry_mode(const Fat32DirEntry *e) {
    uint32_t mode = (e->attr & FAT32_ATTR_DIRECTORY) ? (S_IFDIR | 0755) : (S_IFREG | 0755);
    if (e->attr & FAT32_ATTR_READ_ONLY) mode &= ~0222u;
    return mode;
}

/* The inode for an entry of directory parent (NULL for the root; from the
 * cache, or new). NULL when out of memory. */
static Inode *fat32_iget(Fat32Volume *vol, uint64_t ino, uint32_t mode, uint32_t first_cluster, loff_t size,
                         Inode *parent) {
    Inode *inode = icache_get(&vol->sb, ino);
    if (inode != NULL) return inode;
    Fat32Inode *fi = kmalloc(sizeof(*fi), GFP_ZERO);
    inode = fi != NULL ? inode_new(&vol->sb, ino, mode) : NULL;
    if (inode == NULL) {
        kfree(fi);
        return NULL;
    }
    fi->first_cluster = first_cluster;
    fi->parent_ino = parent != NULL ? parent->ino : ino;
    /* A directory holds its parent (no cycle: a parent holds no reference
     * to its children; only the dcache does, and it lets go) */
    fi->parent = parent != NULL && S_ISDIR(mode) ? igrab(parent) : NULL;
    inode->priv = fi;
    inode->size = S_ISDIR(mode) ? 0 : size;
    inode->blocks = ((uint64_t)size + 511) / 512;
    inode->nlink = S_ISDIR(mode) ? 2 : 1;
    inode->iop = S_ISDIR(mode) ? &fat32_dir_iops : NULL;
    inode->fop = S_ISDIR(mode) ? &fat32_dir_fops : &fat32_file_fops;
    if (!S_ISDIR(mode) && filemap_inode_init(inode, &fat32_aops, 0) != 0) {
        iput(inode);
        return NULL;
    }
    return icache_add(inode);   /* a racing lookup's copy wins; ours is freed */
}

static void fat32_evict(Inode *inode) {
    filemap_inode_evict(inode);   /* a file's cached pages (no reads in flight: each held a reference) */
    Fat32Inode *fi = fi_of(inode);
    if (fi != NULL && fi->parent != NULL) iput(fi->parent);   /* depth of the tree at most: short */
    kfree(fi);
}

static const SuperOps fat32_sops = {
    .evict_inode = fat32_evict,
};

/* --- Directory walk --- */

/* Skip free, end, long-name and volume-label entries. -1 at the end of the
 * directory, 1 to skip, 0 for a real entry. */
static int fat32_entry_filter(const Fat32DirEntry *e) {
    if (e->name[0] == FAT32_DIR_END) return -1;
    if (e->name[0] == FAT32_DIR_FREE) return 1;
    if (e->attr == FAT32_ATTR_LFN) return 1;
    if (e->attr & FAT32_ATTR_VOLUME_ID) return 1;
    return 0;
}

static int is_dot_entry(const Fat32DirEntry *e) {
    return e->name[0] == '.' && (e->name[1] == ' ' || (e->name[1] == '.' && e->name[2] == ' '));
}

/* Called for each real entry from index start on, without io_lock. Returns
 * 0 to go on, nonzero to stop (that value is walk_dir's result). */
typedef int (*DirVisitor)(const Fat32DirEntry *e, uint32_t idx, void *ctx);

/* Walk dir's entries from index start: the visitor's stop value, 0 at the end
 * of the directory, or -errno. */
static int walk_dir(Inode *dir, uint32_t start, DirVisitor visit, void *ctx) {
    Fat32Volume *vol = vol_of(dir);
    uint32_t epc = vol->bytes_per_cluster / sizeof(Fat32DirEntry);
    Fat32DirEntry *buf = kmalloc(vol->bytes_per_cluster, 0);
    if (buf == NULL) return -ENOMEM;
    int rc = 0;
    for (uint32_t ci = start / epc; rc == 0 && ci * epc < FAT32_DIR_MAX_ENTRIES; ci++) {
        int got = read_chain_cluster(dir, ci, buf);
        if (got <= 0) {
            rc = got;
            break;
        }
        for (uint32_t i = ci == start / epc ? start % epc : 0; rc == 0 && i < epc; i++) {
            int f = fat32_entry_filter(&buf[i]);
            if (f < 0) goto done;
            if (f == 0 && !is_dot_entry(&buf[i])) rc = visit(&buf[i], ci * epc + i, ctx);
        }
    }
done:
    kfree(buf);
    return rc;
}

/* --- Lookup --- */

typedef struct LookupCtx {
    const char *name;
    Fat32DirEntry found;
    uint32_t idx;
} LookupCtx;

static int lookup_visitor(const Fat32DirEntry *e, uint32_t idx, void *arg) {
    LookupCtx *ctx = arg;
    if (!fat32_name_match(e, ctx->name)) return 0;
    ctx->found = *e;
    ctx->idx = idx;
    return 1;
}

static int fat32_lookup(Inode *dir, const char *name, Inode **out) {
    if (strcmp(name, "..") == 0) {
        Inode *parent = fi_of(dir)->parent;
        *out = igrab(parent != NULL ? parent : dir);
        return 0;
    }
    LookupCtx ctx = {.name = name};
    int rc = walk_dir(dir, 0, lookup_visitor, &ctx);
    if (rc < 0) return rc;
    if (rc == 0) return -ENOENT;
    Fat32Volume *vol = vol_of(dir);
    uint32_t mode = entry_mode(&ctx.found);
    *out = fat32_iget(vol, entry_ino(fi_of(dir)->first_cluster, ctx.idx), mode, fat32_entry_cluster(&ctx.found),
                      ctx.found.file_size, dir);
    return *out != NULL ? 0 : -ENOMEM;
}

/* --- getdents: cookie = 2 + entry index --- */

typedef struct IterCtx {
    DirContext *dctx;
    uint32_t dir_cluster;
} IterCtx;

static int iter_visitor(const Fat32DirEntry *e, uint32_t idx, void *arg) {
    IterCtx *ctx = arg;
    char name[13];
    fat32_name_to_str(e, name);
    uint8_t dtype = vfs_dtype(entry_mode(e));
    ctx->dctx->pos = (loff_t)idx + 2;
    return dir_emit(ctx->dctx, name, strlen(name), entry_ino(ctx->dir_cluster, idx), dtype, (loff_t)idx + 3) ? 0 : 1;
}

static int fat32_iterate(File *file, DirContext *ctx) {
    Inode *dir = file->inode;
    if (!dir_emit_dots(ctx, dir->ino, fi_of(dir)->parent_ino)) return 0;
    if (ctx->pos - 2 >= FAT32_DIR_MAX_ENTRIES) return 0;
    IterCtx ictx = {.dctx = ctx, .dir_cluster = fi_of(dir)->first_cluster};
    int rc = walk_dir(dir, (uint32_t)(ctx->pos - 2), iter_visitor, &ictx);
    return rc < 0 ? rc : 0;
}

/* --- File data: through the page cache (mm/filemap.h) --- */

/* Start filling one page of a file: one read piece per cluster piece it
 * covers, straight from the device into the page (filemap_bdev_read), and
 * zeros past the end of the file (or of a short chain). Clusters may be
 * smaller or larger than a page; both are sector multiples. io_lock held;
 * the read ends when its last piece does (filemap.h). */
static void fat32_start_page(Inode *inode, struct page *pg) {
    Fat32Volume *vol = vol_of(inode);
    uint64_t base = (uint64_t)pg->index << PAGE_SHIFT;
    uint64_t size = (uint64_t)inode->size;
    uint32_t bpc = vol->bytes_per_cluster;
    size_t off = 0;
    int err = 0;
    filemap_io_begin(pg);
    while (off < PAGE_SIZE && base + off < size) {
        uint64_t at = base + off;
        uint32_t c = chain_cluster(vol, fi_of(inode), (uint32_t)(at / bpc));
        if (c == 0) break;   /* a chain shorter than the size: zeros */
        uint32_t in_cluster = (uint32_t)(at % bpc);
        size_t n = bpc - in_cluster;
        if (n > PAGE_SIZE - off) n = PAGE_SIZE - off;
        uint64_t sector = vol->data_start + (uint64_t)(c - 2) * vol->sectors_per_cluster + in_cluster / SECTOR_SIZE;
        err = filemap_bdev_read(vol->dev, sector, (uint32_t)(n / SECTOR_SIZE), pg, off);
        if (err != 0) break;
        off += n;
    }
    /* Past the end: zeros (the pieces in flight fill only [0, off)) */
    size_t valid = size > base ? (size - base < PAGE_SIZE ? (size_t)(size - base) : PAGE_SIZE) : 0;
    if (valid > off) valid = off;
    memset((uint8_t *)page_to_virt(pg) + valid, 0, PAGE_SIZE - valid);
    filemap_io_end(pg, err);
}

static int fat32_readpage(AddressSpace *as, struct page *pg) {
    Fat32Volume *vol = vol_of(as->host);
    mutex_lock(&vol->io_lock);
    fat32_start_page(as->host, pg);
    mutex_unlock(&vol->io_lock);
    return 0;
}

/* A readahead batch: one hold of the device for all of it, the chain cursor
 * walking forward page by page */
static void fat32_readpages(AddressSpace *as, struct page **pages, uint32_t n) {
    Fat32Volume *vol = vol_of(as->host);
    mutex_lock(&vol->io_lock);
    for (uint32_t i = 0; i < n; i++) fat32_start_page(as->host, pages[i]);
    mutex_unlock(&vol->io_lock);
}

static const AddressSpaceOps fat32_aops = {
    .readpage = fat32_readpage,
    .readpages = fat32_readpages,
};

static const InodeOps fat32_dir_iops = {
    .lookup = fat32_lookup,
};

static const FileOps fat32_dir_fops = {
    .iterate = fat32_iterate,
};

static const FileOps fat32_file_fops = {
    .read = filemap_read,
    .mmap_ops = &filemap_vma_ops,
};

/* --- Mount --- */

/* Check the boot sector and fill vol's geometry. 0 or -EINVAL. */
static int fat32_parse_bpb(Fat32Volume *vol, const Fat32Bpb *bpb) {
    if (bpb->bytes_per_sector != SECTOR_SIZE) {
        kprintf("[FAT32] Unsupported sector size: %u\n", bpb->bytes_per_sector);
        return -EINVAL;
    }
    if (bpb->root_entry_count != 0 || bpb->fat_size_32 == 0 || bpb->sectors_per_cluster == 0 || bpb->num_fats == 0) {
        kprintf("[FAT32] Not a FAT32 boot sector\n");
        return -EINVAL;
    }
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->bytes_per_cluster = (uint32_t)bpb->sectors_per_cluster * SECTOR_SIZE;
    vol->fat_start = bpb->reserved_sectors;
    vol->fat_sectors = bpb->fat_size_32;
    vol->root_cluster = bpb->root_cluster;
    uint64_t data_start = bpb->reserved_sectors + (uint64_t)bpb->num_fats * bpb->fat_size_32;
    if (data_start >= bpb->total_sectors_32) {
        kprintf("[FAT32] Data region past the end of the volume\n");
        return -EINVAL;
    }
    vol->data_start = (uint32_t)data_start;
    vol->total_clusters = (bpb->total_sectors_32 - vol->data_start) / bpb->sectors_per_cluster;
    /* Clusters the FAT can describe: entries 0 and 1 are reserved */
    uint64_t fat_entries = (uint64_t)vol->fat_sectors * SECTOR_SIZE / 4;
    if (fat_entries < 3) return -EINVAL;
    if (vol->total_clusters > fat_entries - 2) vol->total_clusters = (uint32_t)(fat_entries - 2);
    if (!cluster_valid(vol, vol->root_cluster)) {
        kprintf("[FAT32] Bad root cluster %u\n", vol->root_cluster);
        return -EINVAL;
    }
    return 0;
}

SuperBlock *fat32_mount(BlockDevice *dev) {
    if (dev == NULL) return NULL;
    uint8_t sector_buf[SECTOR_SIZE];
    if (dev->read(dev, 0, 1, sector_buf) != 0) {
        kprintf("[FAT32] Failed to read boot sector\n");
        return NULL;
    }
    Fat32Volume *vol = kmalloc(sizeof(Fat32Volume), GFP_ZERO);
    if (vol == NULL) return NULL;
    vol->dev = dev;
    if (fat32_parse_bpb(vol, (const Fat32Bpb *)sector_buf) != 0) {
        kfree(vol);
        return NULL;
    }
    kprintf("[FAT32] BPB: spc=%u fat_start=%u data_start=%u root_cluster=%u total_clusters=%u\n",
            vol->sectors_per_cluster, vol->fat_start, vol->data_start, vol->root_cluster, vol->total_clusters);

    uint64_t fat_bytes = (uint64_t)vol->fat_sectors * SECTOR_SIZE;
    vol->fat = kmalloc((size_t)fat_bytes, 0);
    if (vol->fat == NULL) {
        kprintf("[FAT32] Failed to allocate FAT cache (%lu bytes)\n", fat_bytes);
        kfree(vol);
        return NULL;
    }
    for (uint32_t s = 0; s < vol->fat_sectors; s++) {
        if (dev->read(dev, vol->fat_start + s, 1, (uint8_t *)vol->fat + (uint64_t)s * SECTOR_SIZE) != 0) {
            kprintf("[FAT32] Failed to read FAT sector %u\n", s);
            kfree(vol->fat);
            kfree(vol);
            return NULL;
        }
    }

    vfs_sb_init(&vol->sb, "vfat", &fat32_sops);
    vol->sb.flags = SB_RDONLY;
    vol->sb.bdev = dev;
    mutex_init(&vol->io_lock, LOCK_LEVEL_FS_IO);
    Inode *root = fat32_iget(vol, FAT32_ROOT_INO, S_IFDIR | 0755, vol->root_cluster, 0, NULL);
    if (root == NULL) {
        kfree(vol->fat);
        kfree(vol);
        return NULL;
    }
    vol->sb.root = root;   /* the superblock's reference: volumes are never unmounted */
    kprintf("[FAT32] Volume mounted read-only, FAT cached (%lu bytes)\n", fat_bytes);
    return &vol->sb;
}
