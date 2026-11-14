/* arc_os — ext2 writing (M1-15) on the real ext2, VFS, page cache and
 * write-back sources, over images mkfs.ext2 makes in sparse temporary files
 * mapped shared (ext2_image.c): every scenario ends with the volume shut
 * down (ext2_shutdown) and `e2fsck -fn` clean, most also after a sync while
 * still mounted (a crash then loses nothing structural), and file contents
 * are checked against what debugfs reads off the image.
 *
 * Its own program (test_ext2w_runner), one suite per process: volumes are
 * never unmounted and the VFS has 16 mount slots, and the page arena is
 * larger than the other VFS suites'. The host is single-threaded: a wait
 * for I/O runs the deferred completions and moves the clock (ew_sleep);
 * write-back runs where the tests call wb_sync_all or fsync, or where a
 * write goes over the dirty limit. */

#include <stdarg.h>

#include "test_framework.h"
#include "vfs_env.h"
#include "blk_mem.h"
#include "ext2_image.h"

#include "drivers/blkdev.h"
#include "fs/ext2.h"
#include "fs/ext2_internal.h"
#include "fs/path.h"
#include "fs/vfs_internal.h"
#include "mm/filemap.h"
#include "mm/mm.h"
#include "mm/reclaim.h"
#include "mm/writeback.h"
#include "uapi/dirent.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

#define ARENA_PAGES 8192   /* 32 MiB: 10 000 one-page files pass through it only with reclaim */

/* --- A volume: an image, its disk, its mount --- */

typedef struct {
    HostImage im;
    BlockDevice bdev;
    SuperBlock *sb;
    char mnt[32];
} Vol;

static int sleeps;

/* A wait for I/O or for write-back progress: deferred completions land, the
 * clock moves (bounded waits time out), timers fire. Never write-back from
 * here: the waiter may hold a page lock or the allocation lock the flusher
 * would need (on the kernel the flusher is another thread). */
static void ew_sleep(void) {
    if (++sleeps > 1000000) {
        fprintf(stderr, "ext2w: a wait never ended\n");
        abort();
    }
    blk_mem_run();
    vfs_env_uptime_ms += WB_THROTTLE_MS;
    (void)vfs_env_run_timers();
}

static void env(void) {
    vfs_env_setup();
    vfs_env_sleep_hook = ew_sleep;
}

/* The disks, registered (blk_get) so sync's cache flush reaches them */
static BlockDevice *disks[32];
static unsigned ndisks;

/* A disk with a volatile write cache (the order audit counts what isn't
 * flushed yet) over im */
static void disk_on(BlockDevice *dev, const HostImage *im, uint32_t flags) {
    memset(dev, 0, sizeof(*dev));
    dev->driver_data = im->data;
    dev->nr_sectors = im->size / 512;
    dev->flags = flags | BLK_CACHE;
    dev->disk = dev;
    snprintf(dev->name, sizeof(dev->name), "mem%u", ndisks);
    unsigned i = 0;
    while (i < ndisks && disks[i] != dev) i++;
    if (i == ndisks && ndisks < sizeof(disks) / sizeof(disks[0])) disks[ndisks++] = dev;
    blk_mem_devices(disks, ndisks);
}

/* Mount im at mnt (made here) on a fresh disk. 0, or the error */
static int vol_mount(Vol *v, const char *mnt, uint32_t dev_flags, uint32_t mount_flags) {
    snprintf(v->mnt, sizeof(v->mnt), "%s", mnt);
    disk_on(&v->bdev, &v->im, dev_flags);
    int err;
    v->sb = ext2_mount(&v->bdev, v->im.size, mnt + 1, mount_flags, &err);
    if (v->sb == NULL) return err;
    if (vfs_mkdir(mnt, 0755) != 0 || vfs_mount(mnt, v->sb, mnt + 1) != 0) return -EBUSY;
    return 0;
}

/* A new image of mib MiB (ext2_image.h for opts and tree) mounted read-write at mnt */
static int vol_new(Vol *v, const char *mnt, uint64_t mib, const char *opts, const char *tree) {
    env();
    if (himg_make(&v->im, mib, opts, tree) != 0) return -1;
    int err = vol_mount(v, mnt, 0, EXT2_MOUNT_RW);
    if (err != 0) return err;
    return ext2_writable(v->sb) ? 0 : -EROFS;
}

/* --- The write order (blk_mem.h audit) ---
 *
 * Every write to an ext2 image is classed from the layout on that disk (read
 * here, independently of the kernel's copy): file data, other metadata
 * (directories, indirect and symlink blocks), inode tables, and allocation
 * state (superblock, group descriptors, bitmaps). The rule ext2_alloc.c
 * promises: an inode-table write never starts while file data is in flight
 * or not flushed (an inode never points at data a crash could lose), and an
 * allocation-state write never starts while anything else is (round two
 * after round one, after the data); nor an inode-table write while
 * allocation state is (the rounds never overlap, so a bit freed in round two
 * never reaches the disk before the pointer cleared in round one). Writes ext2 makes on purpose outside a
 * pass (a new indirect or directory block, before anything points at it)
 * are "other metadata", which no rule constrains. */

enum { CLASS_OTHER = 1, CLASS_ITABLE, CLASS_ALLOC };

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int ext2_class(const BlockDevice *dev, uint64_t sector, int file_page) {
    if (file_page) return BLK_MEM_DATA;
    const uint8_t *img = dev->driver_data, *sb = img + 1024;
    if (dev->nr_sectors < 8 || (sb[56] | sb[57] << 8) != 0xEF53) return -1;
    uint32_t bs = 1024u << le32(sb + 24), first = le32(sb + 20), per_group = le32(sb + 32);
    uint32_t ipg = le32(sb + 40), isize = sb[88] | sb[89] << 8, nblocks = le32(sb + 4);
    uint32_t groups = (nblocks - first + per_group - 1) / per_group;
    uint64_t b = sector * 512 / bs;
    uint32_t gdt_end = first + 1 + (groups * 32 + bs - 1) / bs;   /* the descriptor blocks end here */
    if (b == 0 || (b > first && b < gdt_end)) return CLASS_ALLOC;
    const uint8_t *gd = img + (uint64_t)(first + 1) * bs;
    for (uint32_t g = 0; g < groups; g++, gd += 32) {
        uint64_t table = le32(gd + 8);
        if (b == le32(gd) || b == le32(gd + 4)) return CLASS_ALLOC;
        if (b >= table && b < table + (uint64_t)ipg * isize / bs) return CLASS_ITABLE;
    }
    return CLASS_OTHER;
}

static void order_audit_on(void) {
    blk_mem_classify = ext2_class;
    blk_mem_before[CLASS_ITABLE] = 1u << BLK_MEM_DATA | 1u << CLASS_ALLOC;
    blk_mem_before[CLASS_ALLOC] = 1u << BLK_MEM_DATA | 1u << CLASS_OTHER | 1u << CLASS_ITABLE;
    blk_mem_audit = 1;
}

/* 0 if every write so far kept the order, else the first that didn't (once) */
static int order_kept(void) {
    if (blk_mem_order_errors == 0) return 0;
    printf("  write order broken %d times; first: %s\n", blk_mem_order_errors, blk_mem_order_error);
    blk_mem_order_errors = 0;
    return -1;
}

/* Shut the volume down, then e2fsck: 0 if both are clean and every write
 * kept the order */
static int vol_finish(Vol *v) {
    int err = ext2_shutdown(v->sb);
    if (err != 0) {
        printf("  ext2_shutdown: %d\n", err);
        return err;
    }
    int fsck = himg_fsck(&v->im);
    return fsck != 0 ? fsck : order_kept();
}

/* A sync while mounted, then e2fsck on the disk as it is: what a crash right
 * now would leave (0 = clean, and every write kept the order) */
static int synced_disk_clean(Vol *v) {
    wb_sync_all();
    int fsck = himg_fsck(&v->im);
    return fsck != 0 ? fsck : order_kept();
}

static char paths[8][512];
static int path_next;
/* mnt + "/" + printf(fmt): rotating buffers, so a call can use a few */
static const char *at(const Vol *v, const char *fmt, ...) {
    char *p = paths[path_next++ % 8];
    int n = snprintf(p, sizeof(paths[0]), "%s/", v->mnt);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p + n, sizeof(paths[0]) - (size_t)n, fmt, ap);
    va_end(ap);
    return p;
}

/* --- Files --- */

static int64_t pwr(File *f, const void *p, size_t n, loff_t off) {
    IoBuf b = iob_kernel((void *)p, n);
    return vfs_pwrite(f, &b, off);
}

static int64_t prd(File *f, void *p, size_t n, loff_t off) {
    IoBuf b = iob_kernel(p, n);
    return vfs_pread(f, &b, off);
}

static File *open_rw(const char *path) {
    File *f;
    return vfs_open(path, O_RDWR | O_CREAT, 0644, &f) == 0 ? f : NULL;
}

/* The pattern a file of seed holds at byte off */
static uint8_t pb(uint32_t seed, uint64_t off) {
    uint64_t x = (off + 1) * 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed << 32);
    return (uint8_t)((x >> 29) ^ (x >> 47) ^ off);
}

static void fill(uint8_t *buf, uint32_t seed, uint64_t off, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = pb(seed, off + i);
}

/* A whole file read through the VFS equals want[0..len) */
static int same_as(const char *path, const uint8_t *want, uint64_t len) {
    File *f;
    if (vfs_open(path, O_RDONLY, 0, &f) != 0) return 0;
    int ok = (uint64_t)f->inode->size == len;
    static uint8_t chunk[65536];
    for (uint64_t off = 0; ok && off < len; off += sizeof(chunk)) {
        size_t n = len - off < sizeof(chunk) ? (size_t)(len - off) : sizeof(chunk);
        ok = prd(f, chunk, n, (loff_t)off) == (int64_t)n && memcmp(chunk, want + off, n) == 0;
        if (!ok) printf("  %s differs in [%llu, +%zu)\n", path, (unsigned long long)off, n);
    }
    fput(f);
    return ok;
}

/* The file as debugfs reads it off the image equals want[0..len) */
static int on_disk_as(const Vol *v, const char *rel, const uint8_t *want, uint64_t len) {
    uint64_t n;
    uint8_t *got = himg_dump(&v->im, rel, &n);
    int ok = got != NULL && n == len && memcmp(got, want, len) == 0;
    if (!ok)
        printf("  debugfs dump %s: %s, %llu bytes (want %llu)\n", rel, got ? "differs" : "failed",
               (unsigned long long)(got ? n : 0), (unsigned long long)len);
    free(got);
    return ok;
}

static int write_text(const char *path, const char *s) {
    return vfs_write_file(path, s, strlen(s), 0644);
}

/* Entries in a directory, "." and ".." left out; -errno on failure */
typedef struct {
    DirContext ctx;
    int n;
} Count;

static int count_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    (void)ino, (void)dtype, (void)next;
    if (!(len == 1 && name[0] == '.') && !(len == 2 && name[0] == '.' && name[1] == '.')) ((Count *)ctx)->n++;
    return 0;
}

static int entries(const char *path) {
    File *f;
    int err = vfs_open(path, O_RDONLY | O_DIRECTORY, 0, &f);
    if (err != 0) return err;
    Count c = {{0, count_actor}, 0};
    for (;;) {
        loff_t pos = c.ctx.pos;
        err = vfs_iterate(f, &c.ctx);
        if (err != 0 || c.ctx.pos == pos) break;
    }
    fput(f);
    return err != 0 ? err : c.n;
}

static uint32_t nlink_of(const char *path) {
    struct stat st;
    return vfs_stat(path, &st) == 0 ? (uint32_t)st.st_nlink : 0;
}

/* Every cached page and inode gone: what follows reads the disk */
static void drop_all(void) {
    wb_sync_all();
    vfs_drop_caches();
    (void)reclaim_drop_clean();
}

static uint64_t rng_state;
static uint32_t rnd(uint32_t n) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)((rng_state >> 33) % n);
}

/* ========================================================================
 * Suite "files"
 * ===================================================================== */

/* 10 000 files in one directory (it grows into its indirect block): every
 * one readable through the VFS and through debugfs, the directory lists them
 * all, and the disk is consistent after a sync and after shutdown */
TEST(ten_thousand_files) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/w1", 256, NULL, NULL), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "big"), 0755), 0);
    char text[64];
    for (int i = 0; i < 10000; i++) {
        snprintf(text, sizeof(text), "file %d\n", i);
        ASSERT_EQ(write_text(at(&v, "big/f%05d", i), text), 0);
        if (i % 1000 == 999) wb_sync_all();   /* the cache stays mostly clean: reclaim can take it */
    }
    ASSERT_EQ(entries(at(&v, "big")), 10000);
    VfsEnvUsage u;
    vfs_env_usage(&u);
    /* Bounded by the dentry cache, not the file count: a created file stays
     * cached like a looked-up one (its inode and page) until its entry is
     * the least recently used of DCACHE_MAX, then leaves (written first) */
    ASSERT_TRUE(u.pages < DCACHE_MAX + 1024);
    char buf[64];
    ASSERT_EQ(vfs_env_read_file(at(&v, "big/f04711"), buf, sizeof(buf)), 10);
    ASSERT_STR_EQ(buf, "file 4711\n");
    ASSERT_EQ(write_text(at(&v, "big/f00000"), "rewritten\n"), 0);   /* O_TRUNC over a cached name */
    ASSERT_EQ(synced_disk_clean(&v), 0);
    drop_all();
    ASSERT_EQ(entries(at(&v, "big")), 10000);   /* read back off the disk */
    ASSERT_EQ(vfs_env_read_file(at(&v, "big/f09999"), buf, sizeof(buf)), 10);
    ASSERT_STR_EQ(buf, "file 9999\n");
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_TRUE(on_disk_as(&v, "/big/f09998", (const uint8_t *)"file 9998\n", 10));
    ASSERT_TRUE(on_disk_as(&v, "/big/f00000", (const uint8_t *)"rewritten\n", 10));
    char out[256];
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /big/f05000", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "Size: 10") != NULL);
    return 0;
}

#define SHADOW_MAX (12u << 20)

/* Writes, overwrites, extends past holes and truncates (into the direct
 * blocks, the single-indirect range and down to exactly 12 blocks): after
 * each step the file reads as a model of it, and at the end the image holds
 * the same bytes with no block past the end (e2fsck checks i_size and
 * i_blocks against the map) */
TEST(write_overwrite_extend_truncate) {
    static Vol v;
    static uint8_t want[SHADOW_MAX];
    static uint8_t chunk[65536];
    ASSERT_EQ(vol_new(&v, "/w2", 64, NULL, NULL), 0);
    const char *path = at(&v, "data");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    uint64_t len = 0;
    /* Odd-sized appends */
    for (uint64_t off = 0; off < (1u << 20) + 123; off += 3001) {
        fill(chunk, 1, off, 3001);
        ASSERT_EQ(pwr(f, chunk, 3001, (loff_t)off), 3001);
        memcpy(want + off, chunk, 3001);
        len = off + 3001;
    }
    ASSERT_TRUE(same_as(path, want, len));
    /* Overwrite the middle (blocks already there) */
    fill(chunk, 2, 200000, 50000);
    ASSERT_EQ(pwr(f, chunk, 50000, 200000), 50000);
    memcpy(want + 200000, chunk, 50000);
    ASSERT_TRUE(same_as(path, want, len));
    ASSERT_EQ(synced_disk_clean(&v), 0);
    /* A write past the end: the gap is a hole that reads as zeros */
    uint64_t far = (6u << 20) + 5;
    fill(chunk, 3, far, 60000);
    ASSERT_EQ(pwr(f, chunk, 60000, (loff_t)far), 60000);
    memset(want + len, 0, far - len);
    memcpy(want + far, chunk, 60000);
    len = far + 60000;
    ASSERT_TRUE(same_as(path, want, len));
    /* Truncate into the single-indirect range: the tail of the last block
     * reads as zeros when the file grows again */
    ASSERT_EQ(vfs_ftruncate(f, 50007), 0);
    len = 50007;
    ASSERT_TRUE(same_as(path, want, len));
    ASSERT_EQ(synced_disk_clean(&v), 0);
    ASSERT_EQ(vfs_ftruncate(f, 3u << 20), 0);
    memset(want + len, 0, (3u << 20) - len);
    len = 3u << 20;
    ASSERT_EQ(pwr(f, "0123456789", 10, 2u << 20), 10);
    memcpy(want + (2u << 20), "0123456789", 10);
    ASSERT_TRUE(same_as(path, want, len));
    /* Down to exactly the 12 direct blocks: the indirect block goes */
    ASSERT_EQ(vfs_ftruncate(f, 12 * 4096), 0);
    len = 12 * 4096;
    ASSERT_EQ(synced_disk_clean(&v), 0);
    fput(f);
    /* Appends with O_APPEND, write-back in the middle */
    ASSERT_EQ(vfs_open(path, O_WRONLY | O_APPEND, 0, &f), 0);
    for (int i = 0; i < 80; i++) {
        fill(chunk, 4, len, sizeof(chunk));
        IoBuf b = iob_kernel(chunk, sizeof(chunk));
        ASSERT_EQ(vfs_write(f, &b), (int64_t)sizeof(chunk));
        memcpy(want + len, chunk, sizeof(chunk));
        len += sizeof(chunk);
        if (i == 40) wb_sync_all();
    }
    ASSERT_EQ(vfs_fsync(f), 0);
    fput(f);
    ASSERT_TRUE(same_as(path, want, len));
    drop_all();
    ASSERT_TRUE(same_as(path, want, len));   /* off the disk */
    /* A second file: written, truncated to nothing, written again, removed */
    const char *tmp = at(&v, "tmp");
    fill(chunk, 5, 0, sizeof(chunk));
    ASSERT_EQ(vfs_write_file(tmp, chunk, sizeof(chunk), 0644), 0);
    ASSERT_EQ(vfs_write_file(tmp, chunk, 100, 0644), 0);   /* O_TRUNC */
    ASSERT_TRUE(same_as(tmp, chunk, 100));
    ASSERT_EQ(vfs_unlink(tmp), 0);
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_TRUE(on_disk_as(&v, "/data", want, len));
    return 0;
}

/* A file larger than the page arena, written with write-back along the
 * way: reclaim takes its written pages, and every byte reads back, through
 * the cache and off the image */
TEST(file_larger_than_memory) {
    static Vol v;
    static uint8_t chunk[1 << 20], got[1 << 20];
    ASSERT_EQ(vol_new(&v, "/w7", 96, NULL, NULL), 0);
    const char *path = at(&v, "large");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    const uint64_t mib = (ARENA_PAGES * 4096ull >> 20) + 16;
    ReclaimStats r0, r1;
    reclaim_stats(&r0);
    for (uint64_t i = 0; i < mib; i++) {
        fill(chunk, 12, i << 20, sizeof(chunk));
        ASSERT_EQ(pwr(f, chunk, sizeof(chunk), (loff_t)(i << 20)), (int64_t)sizeof(chunk));
        if (i % 4 == 3) wb_sync_all();   /* under the dirty limit: the flusher's job on the kernel */
    }
    reclaim_stats(&r1);
    ASSERT_TRUE(r1.steal_direct > r0.steal_direct);   /* it did not fit */
    for (uint64_t i = 0; i < mib; i++) {
        fill(chunk, 12, i << 20, sizeof(chunk));
        ASSERT_EQ(prd(f, got, sizeof(got), (loff_t)(i << 20)), (int64_t)sizeof(got));
        ASSERT_TRUE(memcmp(got, chunk, sizeof(chunk)) == 0);
    }
    fput(f);
    ASSERT_EQ(vol_finish(&v), 0);
    uint64_t n;
    uint8_t *disk = himg_dump(&v.im, "/large", &n);
    ASSERT_TRUE(disk != NULL);
    ASSERT_EQ(n, mib << 20);
    int same = 1;
    for (uint64_t i = 0; i < mib && same; i++) {
        fill(chunk, 12, i << 20, sizeof(chunk));
        same = memcmp(disk + (i << 20), chunk, sizeof(chunk)) == 0;
    }
    free(disk);
    ASSERT_TRUE(same);
    return 0;
}

/* Blocks at every block-map level of a 5 GiB file with holes between (the
 * last ones triple indirect), read back off the disk; truncated to just over
 * 2 GiB (large_file, the triple-indirect tree freed), then -EFBIG past the
 * largest file a 4 KiB-block volume holds */
TEST(sparse_file_through_triple_indirect) {
    static Vol v;
    static uint8_t buf[4096], got[4096];
    ASSERT_EQ(vol_new(&v, "/w3", 64, NULL, NULL), 0);
    const char *path = at(&v, "sparse");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    const uint64_t offs[] = {0, 48 << 10, (48 << 10) + (4096ull << 10), 1ull << 30, (5ull << 30) - 4096};
    const int n = (int)(sizeof(offs) / sizeof(offs[0]));
    for (int i = 0; i < n; i++) {
        fill(buf, 6, offs[i], sizeof(buf));
        ASSERT_EQ(pwr(f, buf, sizeof(buf), (loff_t)offs[i]), 4096);
    }
    ASSERT_EQ(f->inode->size, (loff_t)(5ull << 30));
    ASSERT_EQ(vfs_fsync(f), 0);
    fput(f);
    ASSERT_EQ(synced_disk_clean(&v), 0);
    drop_all();
    ASSERT_EQ(vfs_open(path, O_RDWR, 0, &f), 0);
    for (int i = 0; i < n; i++) {
        fill(buf, 6, offs[i], sizeof(buf));
        ASSERT_EQ(prd(f, got, sizeof(got), (loff_t)offs[i]), 4096);
        ASSERT_MEM_EQ(got, buf, sizeof(buf));
        if (offs[i] >= 8192) {   /* the page before is a hole */
            ASSERT_EQ(prd(f, got, sizeof(got), (loff_t)offs[i] - 4096), 4096);
            for (int k = 0; k < 4096; k++) ASSERT_EQ(got[k], 0);
        }
    }
    ASSERT_EQ(vfs_ftruncate(f, (2ll << 30) + 1), 0);
    ASSERT_EQ(prd(f, got, sizeof(got), 1ll << 30), 4096);
    fill(buf, 6, 1ull << 30, sizeof(buf));
    ASSERT_MEM_EQ(got, buf, sizeof(buf));
    ASSERT_EQ(synced_disk_clean(&v), 0);
    /* Past the largest file (about 2 TiB on 4 KiB blocks: 512-byte i_blocks) */
    ASSERT_EQ(pwr(f, "x", 1, 1ll << 42), -EFBIG);
    fput(f);
    ASSERT_EQ(vol_finish(&v), 0);
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /sparse", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "Size: 2147483649") != NULL);
    ASSERT_TRUE(himg_debugfs(&v.im, "features", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "large_file") != NULL);
    return 0;
}

/* --- Shared file mappings over ext2 (the fault path of mm/filemap_mmap.c) --- */

#define EW_MAP_AT 0x20000000ull

typedef struct {
    Mm mm;
    Vma vma;
} EwMap;

static void ew_map(EwMap *m, File *f, uint64_t pages) {
    static uint64_t pgd = 0x9000;
    memset(m, 0, sizeof(*m));
    m->mm.pgd = pgd += 0x1000;
    spin_lock_init(&m->mm.pt_lock, LOCK_LEVEL_PAGE_TABLE, "pt");
    m->vma = (Vma){.start = EW_MAP_AT,
                   .end = EW_MAP_AT + pages * PAGE_SIZE,
                   .prot = VMA_READ | VMA_WRITE,
                   .flags = VMA_SHARED | VMA_MAYWRITE,
                   .file = f,
                   .ops = &filemap_vma_ops,
                   .mm = &m->mm};
    filemap_vma_ops.open(&m->vma);
}

/* A store to byte off of the mapping (the write fault, then the store) */
static int ew_store(EwMap *m, uint64_t off, uint8_t byte) {
    uint64_t va = EW_MAP_AT + (off & ~(uint64_t)(PAGE_SIZE - 1));
    int r = filemap_vma_ops.fault(&m->mm, &m->vma, va, MM_ACCESS_WRITE);
    if (r != MM_FAULT_OK) return r;
    struct page *pg = NULL;
    if (!vfs_env_pte(m->mm.pgd, va, &pg, NULL)) return -1;
    ((uint8_t *)page_to_virt(pg))[off & (PAGE_SIZE - 1)] = byte;
    return MM_FAULT_OK;
}

static void ew_unmap(EwMap *m) {
    mm_zap_range(&m->mm, m->vma.start, m->vma.end, MM_ZAP_FLUSH);
    filemap_vma_ops.close(&m->vma);
}

/* Stores through a shared mapping into holes: write-back gives the pages
 * blocks (writepage allocates, nothing did at fault time), msync waits, and
 * the image holds the bytes */
TEST(mmap_stores_into_holes_get_blocks_at_write_back) {
    static Vol v;
    static uint8_t want[16 * 4096];
    ASSERT_EQ(vol_new(&v, "/w4", 64, NULL, NULL), 0);
    const char *path = at(&v, "mapped");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(vfs_ftruncate(f, sizeof(want)), 0);   /* all holes */
    ASSERT_EQ(pwr(f, "head", 4, 4096), 4);          /* page 1 has a block */
    memcpy(want + 4096, "head", 4);
    EwMap m;
    ew_map(&m, f, 16);
    const uint64_t at_off[] = {0, 4100, 5 * 4096 + 17, 15 * 4096 + 4095, 12 * 4096};
    for (size_t i = 0; i < sizeof(at_off) / sizeof(at_off[0]); i++) {
        ASSERT_EQ(ew_store(&m, at_off[i], (uint8_t)(0x41 + i)), MM_FAULT_OK);
        want[at_off[i]] = (uint8_t)(0x41 + i);
    }
    ASSERT_EQ(filemap_vma_ops.sync(&m.vma, m.vma.start, m.vma.end), 0);   /* msync(MS_SYNC) */
    ASSERT_EQ(synced_disk_clean(&v), 0);
    ASSERT_TRUE(on_disk_as(&v, "/mapped", want, sizeof(want)));
    ew_unmap(&m);
    fput(f);
    ASSERT_EQ(vol_finish(&v), 0);
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /mapped", out, sizeof(out)) > 0);
    /* 5 data blocks and the indirect block for page 12 and 15, 8 sectors
     * each: the pages never stored to stayed holes */
    ASSERT_TRUE(strstr(out, "Blockcount: 48") != NULL);
    return 0;
}

/* fsync puts the file's data, then its metadata, on the medium (each with a
 * cache flush): the image read while still mounted has the file, whole and
 * consistent */
TEST(fsync_reaches_the_medium) {
    static Vol v;
    static uint8_t data[50000];
    ASSERT_EQ(vol_new(&v, "/w5", 64, NULL, NULL), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "d"), 0755), 0);
    File *f = open_rw(at(&v, "d/log"));
    ASSERT_TRUE(f != NULL);
    fill(data, 7, 0, sizeof(data));
    ASSERT_EQ(pwr(f, data, sizeof(data), 0), (int64_t)sizeof(data));
    int flushes = blk_mem_flushes;
    ASSERT_EQ(vfs_fsync(f), 0);
    ASSERT_TRUE(blk_mem_flushes >= flushes + 2);   /* after the data, after the metadata */
    ASSERT_TRUE(on_disk_as(&v, "/d/log", data, sizeof(data)));
    ASSERT_EQ(himg_fsck(&v.im), 0);   /* no sync: fsync alone left it consistent */
    fput(f);
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* Rounds of create, write, rename, truncate, unlink, mkdir and rmdir leave
 * nothing allocated once the caches are dropped */
TEST(changes_leak_nothing) {
    static Vol v;
    static uint8_t data[3 * 4096 + 7];
    ASSERT_EQ(vol_new(&v, "/w6", 64, NULL, NULL), 0);
    fill(data, 8, 0, sizeof(data));
    VfsEnvUsage before, after;
    for (int round = 0; round < 4; round++) {
        if (round == 1) {
            drop_all();
            vfs_env_usage(&before);
        }
        ASSERT_EQ(vfs_mkdir(at(&v, "r"), 0755), 0);
        for (int i = 0; i < 20; i++) ASSERT_EQ(vfs_write_file(at(&v, "r/f%d", i), data, sizeof(data), 0644), 0);
        ASSERT_EQ(vfs_rename(at(&v, "r/f3"), at(&v, "r/g3")), 0);
        ASSERT_EQ(vfs_rename(at(&v, "r/f4"), at(&v, "r/f5")), 0);   /* over a file: f5's inode goes */
        File *f = open_rw(at(&v, "r/f6"));
        ASSERT_TRUE(f != NULL);
        ASSERT_EQ(vfs_ftruncate(f, 5000), 0);
        ASSERT_EQ(vfs_unlink(at(&v, "r/f6")), 0);   /* open: freed at the last close */
        ASSERT_EQ(pwr(f, "late", 4, 9000), 4);
        fput(f);
        ASSERT_EQ(
            vfs_symlink_at(NULL, at(&v, "r/s"), "a-target-name-long-enough-to-need-a-block-of-its-own-0123456789"), 0);
        for (int i = 0; i < 20; i++) {
            if (i == 4 || i == 6) continue;
            ASSERT_EQ(vfs_unlink(at(&v, i == 3 ? "r/g%d" : "r/f%d", i)), 0);
        }
        ASSERT_EQ(vfs_unlink(at(&v, "r/s")), 0);
        ASSERT_EQ(vfs_rmdir(at(&v, "r")), 0);
    }
    drop_all();
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    ASSERT_EQ(vol_finish(&v), 0);
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "ls /", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, " r ") == NULL);
    return 0;
}

/* ========================================================================
 * Suite "dirs"
 * ===================================================================== */

#define STORM_FILES 300
#define STORM_DIRS  8

typedef struct {
    int dir;    /* -1: gone (renamed over) */
    int name;   /* d<dir>/n<name> */
} StormFile;

/* Thousands of renames between eight directories, a quarter of them over
 * an existing file: every surviving file still holds its own bytes, each
 * directory lists exactly its files, and replaced inodes are freed */
TEST(rename_storm) {
    static Vol v;
    static StormFile files[STORM_FILES];
    static int owner[STORM_DIRS][4 * STORM_FILES];   /* file index + 1 at d/n, or 0 */
    ASSERT_EQ(vol_new(&v, "/d1", 64, NULL, NULL), 0);
    rng_state = 12345;
    for (int d = 0; d < STORM_DIRS; d++) ASSERT_EQ(vfs_mkdir(at(&v, "d%d", d), 0755), 0);
    char text[32];
    for (int i = 0; i < STORM_FILES; i++) {
        files[i] = (StormFile){i % STORM_DIRS, i};
        owner[i % STORM_DIRS][i] = i + 1;
        snprintf(text, sizeof(text), "%d", i);
        ASSERT_EQ(write_text(at(&v, "d%d/n%d", i % STORM_DIRS, i), text), 0);
    }
    int alive = STORM_FILES;
    for (int step = 0; step < 4000 && alive > 20; step++) {
        int i = (int)rnd(STORM_FILES);
        if (files[i].dir < 0) continue;
        int nd = (int)rnd(STORM_DIRS), nn;
        int over = rnd(4) == 0;
        if (over) {   /* onto another live file */
            int j = (int)rnd(STORM_FILES);
            if (files[j].dir < 0 || j == i) continue;
            nd = files[j].dir;
            nn = files[j].name;
        } else {
            nn = (int)rnd(4 * STORM_FILES);
            if (owner[nd][nn] != 0 && owner[nd][nn] != i + 1) continue;   /* taken: an "over" case */
        }
        ASSERT_EQ(vfs_rename(at(&v, "d%d/n%d", files[i].dir, files[i].name), at(&v, "d%d/n%d", nd, nn)), 0);
        int was = owner[nd][nn];
        if (was != 0 && was != i + 1) {
            files[was - 1].dir = -1;
            alive--;
        }
        owner[files[i].dir][files[i].name] = 0;
        owner[nd][nn] = i + 1;
        files[i] = (StormFile){nd, nn};
        if (step == 2000) ASSERT_EQ(synced_disk_clean(&v), 0);
    }
    int per_dir[STORM_DIRS] = {0};
    char buf[32];
    for (int i = 0; i < STORM_FILES; i++) {
        if (files[i].dir < 0) continue;
        per_dir[files[i].dir]++;
        snprintf(text, sizeof(text), "%d", i);
        ASSERT_TRUE(vfs_env_read_file(at(&v, "d%d/n%d", files[i].dir, files[i].name), buf, sizeof(buf)) > 0);
        ASSERT_STR_EQ(buf, text);
    }
    drop_all();
    for (int d = 0; d < STORM_DIRS; d++) ASSERT_EQ(entries(at(&v, "d%d", d)), per_dir[d]);
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* Directory renames: moved between parents (link counts follow, ".." is
 * rewritten), renamed onto an empty directory, refused into its own subtree,
 * onto a non-empty directory, onto a file and a file onto it */
TEST(directory_renames_and_link_counts) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/d2", 64, NULL, NULL), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "a"), 0755), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "a/b"), 0755), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "a/b/c"), 0755), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "x"), 0755), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "empty"), 0755), 0);
    ASSERT_EQ(write_text(at(&v, "a/b/c/file"), "deep\n"), 0);
    ASSERT_EQ(write_text(at(&v, "plain"), "plain\n"), 0);
    uint32_t root_links = nlink_of(v.mnt);
    ASSERT_EQ(nlink_of(at(&v, "a")), 3);
    ASSERT_EQ(vfs_rename(at(&v, "a/b"), at(&v, "x/b")), 0);   /* between parents */
    ASSERT_EQ(nlink_of(at(&v, "a")), 2);
    ASSERT_EQ(nlink_of(at(&v, "x")), 3);
    char buf[16];
    ASSERT_EQ(vfs_env_read_file(at(&v, "x/b/c/../c/file"), buf, sizeof(buf)), 5);
    ASSERT_EQ(vfs_rename(at(&v, "x"), at(&v, "x/b/c/inside")), -EINVAL);   /* into itself */
    ASSERT_EQ(vfs_rename(at(&v, "a"), at(&v, "x")), -ENOTEMPTY);
    ASSERT_EQ(vfs_rename(at(&v, "a"), at(&v, "plain")), -ENOTDIR);
    ASSERT_EQ(vfs_rename(at(&v, "plain"), at(&v, "a")), -EISDIR);
    ASSERT_EQ(vfs_rename(at(&v, "x/b"), at(&v, "empty")), 0);   /* onto an empty directory: it goes */
    ASSERT_EQ(nlink_of(at(&v, "x")), 2);
    ASSERT_EQ(nlink_of(v.mnt), root_links - 0);   /* "empty" replaced: same number of subdirectories */
    ASSERT_EQ(vfs_rmdir(at(&v, "empty")), -ENOTEMPTY);
    ASSERT_EQ(vfs_rename(at(&v, "empty/c"), at(&v, "c2")), 0);   /* up to the root */
    ASSERT_EQ(nlink_of(v.mnt), root_links + 1);
    ASSERT_EQ(vfs_env_read_file(at(&v, "c2/file"), buf, sizeof(buf)), 5);
    ASSERT_EQ(vfs_rename(at(&v, "c2/file"), at(&v, "c2/file2")), 0);   /* within one directory */
    ASSERT_EQ(synced_disk_clean(&v), 0);
    ASSERT_EQ(vfs_unlink(at(&v, "c2/file2")), 0);
    ASSERT_EQ(vfs_rmdir(at(&v, "c2")), 0);
    ASSERT_EQ(nlink_of(v.mnt), root_links);
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* A path 64 directories deep and a wide tree of 40 x 40 files; non-empty
 * directories refuse rmdir; half the wide tree is removed bottom-up */
TEST(deep_and_wide_trees) {
    static Vol v;
    static char deep[1024];
    ASSERT_EQ(vol_new(&v, "/d3", 64, NULL, NULL), 0);
    int n = snprintf(deep, sizeof(deep), "%s", v.mnt);
    for (int i = 0; i < 64; i++) {
        n += snprintf(deep + n, sizeof(deep) - (size_t)n, "/d%02d", i);
        ASSERT_EQ(vfs_mkdir(deep, 0755), 0);
    }
    snprintf(deep + n, sizeof(deep) - (size_t)n, "/bottom");
    ASSERT_EQ(write_text(deep, "bottom\n"), 0);
    for (int d = 0; d < 40; d++) {
        ASSERT_EQ(vfs_mkdir(at(&v, "w%02d", d), 0755), 0);
        for (int i = 0; i < 40; i++) ASSERT_EQ(write_text(at(&v, "w%02d/f%02d", d, i), "x"), 0);
    }
    ASSERT_EQ(vfs_rmdir(at(&v, "w00")), -ENOTEMPTY);
    ASSERT_EQ(vfs_rmdir(at(&v, "d00")), -ENOTEMPTY);
    for (int d = 0; d < 40; d += 2) {
        for (int i = 0; i < 40; i++) ASSERT_EQ(vfs_unlink(at(&v, "w%02d/f%02d", d, i)), 0);
        ASSERT_EQ(vfs_rmdir(at(&v, "w%02d", d)), 0);
    }
    ASSERT_EQ(synced_disk_clean(&v), 0);
    drop_all();
    char buf[16];
    ASSERT_EQ(vfs_env_read_file(deep, buf, sizeof(buf)), 7);
    ASSERT_EQ(entries(v.mnt), 1 + 20 + 1);   /* d00, the odd wNN, lost+found */
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* A directory with an htree index (made by e2fsck -D, as Linux leaves
 * them): changed by arc_os, it loses the index flag, so Linux and e2fsck
 * read it linearly and find every entry */
TEST(indexed_directory_loses_its_index) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/d4", 64, NULL,
                      "mkdir many && for i in $(seq 1 400); do echo $i > many/file_with_a_long_name_$i; done"),
              0);
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /many", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "Flags: 0x1000") != NULL);   /* indexed to start with */
    ASSERT_EQ(entries(at(&v, "many")), 400);
    for (int i = 401; i <= 500; i++) ASSERT_EQ(write_text(at(&v, "many/file_with_a_long_name_%d", i), "new\n"), 0);
    for (int i = 1; i <= 400; i += 8) ASSERT_EQ(vfs_unlink(at(&v, "many/file_with_a_long_name_%d", i)), 0);
    ASSERT_EQ(entries(at(&v, "many")), 450);
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /many", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "Flags: 0x0") != NULL);
    ASSERT_TRUE(himg_debugfs(&v.im, "cat /many/file_with_a_long_name_500", out, sizeof(out)) == 4);
    ASSERT_STR_EQ(out, "new\n");
    return 0;
}

/* Symlinks: up to 59 bytes in the inode (fast), 60 and up in a block of
 * their own (slow, freed with the link); targets stop at the VFS's PATH_MAX
 * (512 here, fs/path.h) */
TEST(fast_and_slow_symlinks) {
    static Vol v;
    static char target[4096];
    ASSERT_EQ(vol_new(&v, "/d5", 64, NULL, NULL), 0);
    const size_t lens[] = {1, 59, 60, 200, PATH_MAX - 1};
    for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
        for (size_t i = 0; i < lens[k]; i++) target[i] = (char)('a' + (i + k) % 26);
        target[lens[k]] = '\0';
        ASSERT_EQ(vfs_symlink_at(NULL, at(&v, "l%zu", lens[k]), target), 0);
        char got[4096];
        ASSERT_EQ(vfs_readlink_at(NULL, at(&v, "l%zu", lens[k]), got, sizeof(got)), (int)lens[k]);
        ASSERT_TRUE(memcmp(got, target, lens[k]) == 0);
    }
    ASSERT_EQ(vfs_symlink_at(NULL, at(&v, "l1"), "x"), -EEXIST);
    memset(target, 'z', PATH_MAX);
    target[PATH_MAX] = '\0';
    ASSERT_EQ(vfs_symlink_at(NULL, at(&v, "toolong"), target), -ENAMETOOLONG);
    ASSERT_EQ(vfs_unlink(at(&v, "l200")), 0);   /* its block goes back */
    ASSERT_EQ(synced_disk_clean(&v), 0);
    drop_all();
    char got[4096];
    ASSERT_EQ(vfs_readlink_at(NULL, at(&v, "l%d", PATH_MAX - 1), got, sizeof(got)), PATH_MAX - 1);
    ASSERT_EQ(vol_finish(&v), 0);
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /l59", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "Fast link dest") != NULL);
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /l60", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "Fast link dest") == NULL && strstr(out, "BLOCKS") != NULL);
    return 0;
}

/* ========================================================================
 * Suite "space"
 * ===================================================================== */

/* Free blocks of the mounted volume, from statfs-like counters debugfs reads
 * off the synced image */
static long free_blocks_on_disk(Vol *v) {
    char out[8192];
    wb_sync_all();
    if (himg_debugfs(&v->im, "stats", out, sizeof(out)) <= 0) return -1;
    const char *p = strstr(out, "Free blocks:");
    return p != NULL ? strtol(p + 12, NULL, 10) : -1;
}

/* A file grows until the volume is full: the write stops short (or gets
 * -ENOSPC), what was written reads back, no block lies past the end, the
 * disk stays consistent; space comes back with the file. A store through a
 * mapping into a hole on the full volume fails at write-back: fsync says
 * -ENOSPC once and the page is dropped */
TEST(full_volume_enospc) {
    static Vol v;
    static uint8_t chunk[1 << 20];
    ASSERT_EQ(vol_new(&v, "/s1", 16, "-m 0", NULL), 0);
    long free0 = free_blocks_on_disk(&v);
    ASSERT_TRUE(free0 > 1000);
    const char *path = at(&v, "fill");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    uint64_t len = 0;
    for (;;) {
        fill(chunk, 9, len, sizeof(chunk));
        int64_t r = pwr(f, chunk, sizeof(chunk), (loff_t)len);
        if (r < 0) {
            ASSERT_EQ(r, -ENOSPC);
            break;
        }
        len += (uint64_t)r;
        if (r < (int64_t)sizeof(chunk)) break;   /* short: the next one says ENOSPC */
    }
    ASSERT_EQ(pwr(f, chunk, 4096, (loff_t)len), -ENOSPC);
    ASSERT_EQ(f->inode->size, (loff_t)len);
    ASSERT_TRUE(len % 4096 == 0);
    /* Data blocks plus the indirect blocks the map needed fill the volume */
    uint64_t blocks = len / 4096;
    uint64_t ind = blocks > 12 ? 1 + (blocks > 12 + 1024 ? 1 + (blocks - 12 - 1024 + 1023) / 1024 : 0) : 0;
    ASSERT_EQ((long)(blocks + ind), free0);
    ASSERT_EQ(free_blocks_on_disk(&v), 0);
    ASSERT_EQ(himg_fsck(&v.im), 0);
    /* Nothing else fits: a new file's first block, a new directory */
    File *g = open_rw(at(&v, "small"));
    ASSERT_TRUE(g != NULL);   /* the inode needs no block */
    ASSERT_EQ(pwr(g, "x", 1, 0), -ENOSPC);
    ASSERT_EQ(g->inode->size, 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "newdir"), 0755), -ENOSPC);
    /* A hole in a mapped file on the full volume: write-back can't place it */
    ASSERT_EQ(vfs_ftruncate(g, 8192), 0);
    EwMap m;
    ew_map(&m, g, 2);
    ASSERT_EQ(ew_store(&m, 5, 'm'), MM_FAULT_OK);
    ASSERT_EQ(filemap_vma_ops.sync(&m.vma, m.vma.start, m.vma.end), -ENOSPC);
    ASSERT_EQ(vfs_fsync(g), -ENOSPC);   /* the kept error, then cleared */
    ASSERT_EQ(vfs_fsync(g), 0);
    ew_unmap(&m);
    fput(g);
    ASSERT_EQ(synced_disk_clean(&v), 0);
    /* Space comes back */
    ASSERT_EQ(vfs_ftruncate(f, 0), 0);
    fput(f);
    ASSERT_EQ(vfs_unlink(path), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "newdir"), 0755), 0);
    ASSERT_EQ(write_text(at(&v, "newdir/after"), "space again\n"), 0);
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_TRUE(on_disk_as(&v, "/newdir/after", (const uint8_t *)"space again\n", 12));
    return 0;
}

/* Inodes run out before blocks: create and mkdir get -ENOSPC, one unlink
 * makes room for one more */
TEST(inode_exhaustion) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/s2", 16, "-N 32", NULL), 0);
    int made = 0;
    for (;; made++) {
        int err = write_text(at(&v, "i%d", made), "i");
        if (err != 0) {
            ASSERT_EQ(err, -ENOSPC);
            break;
        }
        ASSERT_TRUE(made < 64);
    }
    ASSERT_EQ(made, 32 - 11);   /* inodes 1-10 reserved, 11 is lost+found */
    ASSERT_EQ(vfs_mkdir(at(&v, "nodir"), 0755), -ENOSPC);
    ASSERT_EQ(vfs_symlink_at(NULL, at(&v, "nolink"), "i0"), -ENOSPC);
    struct stat st;
    ASSERT_EQ(vfs_stat(at(&v, "i%d", made), &st), -ENOENT);   /* the failed create left no name */
    ASSERT_EQ(synced_disk_clean(&v), 0);
    ASSERT_EQ(vfs_unlink(at(&v, "i5")), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "nodir"), 0755), 0);
    ASSERT_EQ(write_text(at(&v, "one_more"), "x"), -ENOSPC);
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* ========================================================================
 * Suite "state"
 * ===================================================================== */

#define SB_AT 1024
static uint16_t sb_u16(const HostImage *im, unsigned off) {
    uint16_t x;
    memcpy(&x, im->data + SB_AT + off, 2);
    return x;
}
#define S_MNT_COUNT 52
#define S_STATE     58

/* A read-write mount says "in use" on the disk at once (state not clean,
 * mount count up); shutdown puts "clean" back. The volume mounts again,
 * read-write, and reads what the first mount wrote. */
TEST(mount_marks_in_use_shutdown_marks_clean) {
    static Vol v, again;
    env();
    ASSERT_EQ(himg_make(&v.im, 64, NULL, NULL), 0);
    ASSERT_EQ(sb_u16(&v.im, S_STATE), 1);
    uint16_t count = sb_u16(&v.im, S_MNT_COUNT);
    ASSERT_EQ(vol_mount(&v, "/t1", 0, EXT2_MOUNT_RW), 0);
    ASSERT_TRUE(ext2_writable(v.sb));
    ASSERT_EQ(sb_u16(&v.im, S_STATE), 0);
    ASSERT_EQ(sb_u16(&v.im, S_MNT_COUNT), (uint16_t)(count + 1));
    ASSERT_EQ(write_text(at(&v, "kept"), "first mount\n"), 0);
    ASSERT_EQ(ext2_shutdown(v.sb), 0);
    ASSERT_EQ(sb_u16(&v.im, S_STATE), 1);
    ASSERT_FALSE(ext2_writable(v.sb));
    ASSERT_EQ(write_text(at(&v, "late"), "x"), -EROFS);
    ASSERT_EQ(ext2_shutdown(v.sb), 0);   /* again: nothing to do */
    ASSERT_EQ(himg_fsck(&v.im), 0);
    /* The same disk mounted again */
    again.im = v.im;
    ASSERT_EQ(vol_mount(&again, "/t1b", 0, EXT2_MOUNT_RW), 0);
    ASSERT_EQ(sb_u16(&again.im, S_MNT_COUNT), (uint16_t)(count + 2));
    char buf[32];
    ASSERT_EQ(vfs_env_read_file(at(&again, "kept"), buf, sizeof(buf)), 12);
    ASSERT_STR_EQ(buf, "first mount\n");
    ASSERT_EQ(write_text(at(&again, "second"), "second mount\n"), 0);
    ASSERT_EQ(vol_finish(&again), 0);
    ASSERT_EQ(sb_u16(&again.im, S_STATE), 1);
    ASSERT_TRUE(on_disk_as(&again, "/second", (const uint8_t *)"second mount\n", 13));
    return 0;
}

/* A volume found not clean is mounted read-write (with a warning; e2fsck at
 * boot comes later) and shut down still not clean: arc_os never marks clean
 * a volume it did not check */
TEST(dirty_volume_stays_marked_dirty) {
    static Vol v;
    env();
    ASSERT_EQ(himg_make(&v.im, 64, NULL, NULL), 0);
    v.im.data[SB_AT + S_STATE] = 0;   /* as a crash leaves it */
    ASSERT_EQ(vol_mount(&v, "/t2", 0, EXT2_MOUNT_RW), 0);
    ASSERT_TRUE(ext2_writable(v.sb));
    ASSERT_EQ(write_text(at(&v, "f"), "written anyway\n"), 0);
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_EQ(sb_u16(&v.im, S_STATE), 0);
    return 0;
}

/* Volumes arc_os must not write: 1 KiB blocks, an unknown read-only-
 * compatible feature, a read-only disk. Asked for read-write, each mounts
 * read-only and the image is not changed by one byte (ADR-0018). */
TEST(volumes_it_must_not_write_stay_untouched) {
    static Vol v[3];
    const char *mnt[3] = {"/t3a", "/t3b", "/t3c"};
    env();
    ASSERT_EQ(himg_make(&v[0].im, 16, "-b 1024 -I 128", NULL), 0);
    ASSERT_EQ(himg_make(&v[1].im, 16, NULL, NULL), 0);
    ASSERT_EQ(himg_make(&v[2].im, 16, NULL, NULL), 0);
    uint32_t ro;
    memcpy(&ro, v[1].im.data + SB_AT + 100, 4);   /* s_feature_ro_compat */
    ro |= 0x8000;
    memcpy(v[1].im.data + SB_AT + 100, &ro, 4);
    for (int i = 0; i < 3; i++) {
        uint8_t *copy = malloc(v[i].im.size);
        ASSERT_TRUE(copy != NULL);
        memcpy(copy, v[i].im.data, v[i].im.size);
        ASSERT_EQ(vol_mount(&v[i], mnt[i], i == 2 ? BLK_RDONLY : 0, EXT2_MOUNT_RW), 0);
        ASSERT_FALSE(ext2_writable(v[i].sb));
        ASSERT_EQ(ext2_ro_forced(v[i].sb), i == 1);
        ASSERT_EQ(write_text(at(&v[i], "nope"), "x"), -EROFS);
        ASSERT_EQ(vfs_mkdir(at(&v[i], "nodir"), 0755), -EROFS);
        ASSERT_EQ(entries(at(&v[i], "lost+found")), 0);
        ASSERT_EQ(ext2_shutdown(v[i].sb), 0);
        ASSERT_TRUE(memcmp(copy, v[i].im.data, v[i].im.size) == 0);
        free(copy);
    }
    return 0;
}

/* Write-back with the disk answering late (completions arrive as
 * interrupts would): pages stay under write-back until then; a write(2) to
 * one goes ahead (file data need not hold still) and leaves it dirty for the
 * next write; fsync waits for the write in flight, then writes it again */
TEST(write_back_completes_asynchronously) {
    static Vol v;
    static uint8_t data[8 * 4096];
    ASSERT_EQ(vol_new(&v, "/t4", 64, NULL, NULL), 0);
    const char *path = at(&v, "async");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    fill(data, 10, 0, sizeof(data));
    ASSERT_EQ(pwr(f, data, sizeof(data), 0), (int64_t)sizeof(data));
    ASSERT_EQ(vfs_fsync(f), 0);   /* the blocks exist; the next writes only overwrite */
    fill(data, 11, 0, sizeof(data));
    ASSERT_EQ(pwr(f, data, sizeof(data), 0), (int64_t)sizeof(data));
    AddressSpace *as = filemap_of(f->inode);
    blk_mem_defer = 1;
    int r0 = blk_mem_requests;
    ASSERT_EQ(filemap_start_all(as), 0);
    unsigned off;
    struct page *pg = filemap_find(as, 3, &off);
    ASSERT_TRUE(pg != NULL);
    ASSERT_TRUE(page_test(pg, PG_WRITEBACK) && !page_test(pg, PG_DIRTY) && !page_test(pg, PG_LOCKED));
    ASSERT_EQ(blk_mem_requests, r0);   /* all still in flight */
    int s0 = sleeps;
    data[3 * 4096] ^= 0xFF;
    ASSERT_EQ(pwr(f, data + 3 * 4096, 1, 3 * 4096), 1);
    ASSERT_EQ(sleeps, s0);   /* no wait */
    ASSERT_TRUE(page_test(pg, PG_WRITEBACK) && page_test(pg, PG_DIRTY));
    ASSERT_EQ(vfs_fsync(f), 0);   /* waits for the old write (it lands), writes the page again */
    ASSERT_TRUE(sleeps > s0);
    ASSERT_TRUE(!page_test(pg, PG_WRITEBACK) && !page_test(pg, PG_DIRTY));
    page_put(pg);
    blk_mem_defer = 0;
    ASSERT_TRUE(on_disk_as(&v, "/async", data, sizeof(data)));
    fput(f);
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

TestCase ext2w_files_tests[] = {
    TEST_ENTRY(ten_thousand_files),
    TEST_ENTRY(write_overwrite_extend_truncate),
    TEST_ENTRY(file_larger_than_memory),
    TEST_ENTRY(sparse_file_through_triple_indirect),
    TEST_ENTRY(mmap_stores_into_holes_get_blocks_at_write_back),
    TEST_ENTRY(fsync_reaches_the_medium),
    TEST_ENTRY(changes_leak_nothing),
};
TestCase ext2w_dirs_tests[] = {
    TEST_ENTRY(rename_storm),           TEST_ENTRY(directory_renames_and_link_counts),
    TEST_ENTRY(deep_and_wide_trees),    TEST_ENTRY(indexed_directory_loses_its_index),
    TEST_ENTRY(fast_and_slow_symlinks),
};
TestCase ext2w_space_tests[] = {
    TEST_ENTRY(full_volume_enospc),
    TEST_ENTRY(inode_exhaustion),
};
TestCase ext2w_state_tests[] = {
    TEST_ENTRY(mount_marks_in_use_shutdown_marks_clean),
    TEST_ENTRY(dirty_volume_stays_marked_dirty),
    TEST_ENTRY(volumes_it_must_not_write_stay_untouched),
    TEST_ENTRY(write_back_completes_asynchronously),
};

/* ========================================================================
 * Suite "regress": the PR #49 review's findings (each failed before its fix)
 * and writes racing changes (completions deferred until a thread waits)
 * ===================================================================== */

/* A device node's i_block holds its device number, not blocks: unlinking it
 * must not free "block" rdev (made to be another file's data block) */
TEST(unlinked_device_node_frees_no_blocks) {
    static Vol v;
    /* 4 << 8 | 13 = 1037: the first data block mkfs gives, keep's */
    ASSERT_EQ(vol_new(&v, "/rg1", 64, NULL, "mknod node c 4 13 && echo hi > keep"), 0);
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "icheck 1037", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, "1037\t12") != NULL || strstr(out, "1037 12") != NULL);   /* keep (inode 12) owns it */
    ASSERT_EQ(vfs_unlink(at(&v, "node")), 0);
    static uint8_t nn[4096];
    memset(nn, 'N', sizeof(nn));
    ASSERT_EQ(vfs_write_file(at(&v, "new"), nn, sizeof(nn), 0644), 0);
    ASSERT_EQ(synced_disk_clean(&v), 0);   /* no block claimed twice */
    drop_all();
    char buf[8];
    ASSERT_EQ(vfs_env_read_file(at(&v, "keep"), buf, sizeof(buf)), 3);
    ASSERT_TRUE(memcmp(buf, "hi\n", 3) == 0);
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* A store through a mapping past the end, in the last page, must not reach
 * the disk: after the page leaves memory, growing the file shows zeros */
TEST(stores_past_the_end_never_reach_the_disk) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/rg2", 64, NULL, NULL), 0);
    const char *path = at(&v, "tail");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    static uint8_t head[100];
    memset(head, 'A', sizeof(head));
    ASSERT_EQ(pwr(f, head, sizeof(head), 0), 100);
    EwMap m;
    ew_map(&m, f, 1);
    ASSERT_EQ(ew_store(&m, 200, 0x5A), MM_FAULT_OK);   /* past the end, inside the last page (POSIX allows it) */
    ASSERT_EQ(filemap_vma_ops.sync(&m.vma, m.vma.start, m.vma.end), 0);
    ew_unmap(&m);
    fput(f);
    drop_all();   /* the page leaves memory */
    f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(vfs_ftruncate(f, 4096), 0);
    uint8_t got[4096];
    ASSERT_EQ(prd(f, got, sizeof(got), 0), 4096);
    fput(f);
    ASSERT_EQ(got[200], 0);
    ASSERT_TRUE(memcmp(got, head, sizeof(head)) == 0);
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* A linked inode leaving memory writes its dirty pages back while still in
 * the inode cache: a lookup meanwhile (here: while that write waits) finds
 * it and its pages, not a second Inode reading the unwritten disk */
static const char *evict_path;
static uint8_t evict_got[64];
static int evict_ran;
static void evict_hook(void) {
    if (evict_path != NULL && !evict_ran) {
        evict_ran = 1;
        File *f;
        if (vfs_open(evict_path, O_RDONLY, 0, &f) == 0) {
            (void)prd(f, evict_got, sizeof(evict_got), 0);
            fput(f);
        } else {
            evict_ran = -1;
        }
    }
    ew_sleep();
}

TEST(lookup_while_leaving_memory_finds_the_pages) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/rg3", 64, NULL, NULL), 0);
    const char *path = at(&v, "evicted");
    File *f = open_rw(path);
    ASSERT_TRUE(f != NULL);
    static uint8_t data[64];
    memset(data, 'D', sizeof(data));
    ASSERT_EQ(pwr(f, data, sizeof(data), 0), 64);
    static char keep[64];
    snprintf(keep, sizeof(keep), "%s", path);
    evict_path = keep;
    vfs_env_sleep_hook = evict_hook;
    blk_mem_defer = 1;
    fput(f);
    vfs_drop_caches();   /* the last reference (the dentry's): the dirty page is written back */
    blk_mem_defer = 0;
    vfs_env_sleep_hook = ew_sleep;
    evict_path = NULL;
    ASSERT_EQ(evict_ran, 1);
    ASSERT_TRUE(memcmp(evict_got, data, sizeof(data)) == 0);
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_TRUE(on_disk_as(&v, "/evicted", data, sizeof(data)));
    return 0;
}

/* A file with an extended-attribute block (a Linux-made volume) unlinked:
 * the block goes back to the bitmap (e2fsck: no bitmap difference) */
TEST(unlink_gives_back_the_attribute_block) {
    static Vol v;
    env();
    ASSERT_EQ(himg_make(&v.im, 64, NULL, "echo hi > x && echo hi > y"), 0);
    static char cmd[4096];
    char val[1601];
    memset(val, 'v', 1600);
    val[1600] = 0;
    /* Too big for the inode's own space: an external block (y has none) */
    snprintf(cmd, sizeof(cmd), "debugfs -w -R 'ea_set /x user.big %s' '%s' >/dev/null 2>&1", val, v.im.path);
    ASSERT_EQ(system(cmd), 0);
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "stat /x", out, sizeof(out)) > 0);
    const char *fa = strstr(out, "File ACL: ");
    ASSERT_TRUE(fa != NULL && atoi(fa + 10) > 0);
    ASSERT_EQ(vol_mount(&v, "/rg4", 0, EXT2_MOUNT_RW), 0);
    ASSERT_EQ(vfs_unlink(at(&v, "x")), 0);
    ASSERT_EQ(vfs_unlink(at(&v, "y")), 0);   /* no attribute block: nothing to do */
    ASSERT_EQ(vol_finish(&v), 0);
    return 0;
}

/* ftruncate while the truncated pages' writes are in flight: it waits for
 * them, so a file that gets the freed blocks next is never overwritten by
 * the old write landing late */
TEST(truncate_waits_for_writes_in_flight) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/rg5", 64, NULL, NULL), 0);
    static uint8_t a[8192], b[8192];
    memset(a, 'A', sizeof(a));
    memset(b, 'B', sizeof(b));
    File *f = open_rw(at(&v, "a"));
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(pwr(f, a, sizeof(a), 0), (int64_t)sizeof(a));   /* blocks given at write() */
    int64_t old_block = ext2_bmap(f->inode, 0);
    ASSERT_TRUE(old_block > 0);
    blk_mem_defer = 1;   /* the writes stay in flight until a thread waits */
    ASSERT_EQ(filemap_start_range_if(filemap_of(f->inode), 0, UINT64_MAX, NULL, NULL), 0);
    ASSERT_EQ(vfs_ftruncate(f, 0), 0);   /* frees the blocks */
    File *g = open_rw(at(&v, "b"));
    ASSERT_TRUE(g != NULL);
    ASSERT_EQ(pwr(g, b, sizeof(b), 0), (int64_t)sizeof(b));
    ASSERT_EQ(ext2_bmap(g->inode, 0), old_block);   /* the race needs b on a's old block */
    ASSERT_EQ(vfs_fsync(g), 0);
    blk_mem_defer = 0;
    blk_mem_run();   /* anything still deferred lands now: last */
    fput(f);
    fput(g);
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_TRUE(on_disk_as(&v, "/b", b, sizeof(b)));
    return 0;
}

/* A directory changed while its block's write is in flight: the change
 * waits for the write (stable metadata), so the disk gets the block as it
 * was when the write started, never a half-changed one */
TEST(directory_block_holds_still_while_written) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/rg6", 64, NULL, NULL), 0);
    ASSERT_EQ(vfs_mkdir(at(&v, "d"), 0755), 0);
    ASSERT_EQ(write_text(at(&v, "d/x"), "x\n"), 0);
    wb_sync_all();
    ASSERT_EQ(write_text(at(&v, "d/z"), "z\n"), 0);   /* d's block dirty, with z */
    blk_mem_audit = 0;   /* metadata written in no particular order on purpose */
    blk_mem_defer = 1;
    Ext2Vol *ev = (Ext2Vol *)v.sb;
    ASSERT_EQ(filemap_start_range_if(&ev->meta, 0, UINT64_MAX, NULL, NULL), 0);
    ASSERT_EQ(vfs_unlink(at(&v, "d/z")), 0);   /* waits for the block's write first */
    blk_mem_defer = 0;
    blk_mem_run();
    blk_mem_audit = 1;
    char out[4096];
    ASSERT_TRUE(himg_debugfs(&v.im, "ls /d", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, ") z") != NULL);   /* the block as its write started: z still there */
    ASSERT_EQ(vol_finish(&v), 0);
    ASSERT_TRUE(himg_debugfs(&v.im, "ls /d", out, sizeof(out)) > 0);
    ASSERT_TRUE(strstr(out, ") x") != NULL && strstr(out, ") z") == NULL);   /* the unlink, once written */
    return 0;
}

/* A page dirtied through a mapping after the volume was shut down (read-only
 * from then on) is given up at its first failed write, the error kept for
 * fsync, instead of being tried again by every pass */
TEST(page_dirtied_after_shutdown_is_dropped) {
    static Vol v;
    ASSERT_EQ(vol_new(&v, "/rg7", 64, NULL, NULL), 0);
    File *f = open_rw(at(&v, "late"));
    ASSERT_TRUE(f != NULL);
    static uint8_t page[4096];
    memset(page, 'a', sizeof(page));
    ASSERT_EQ(pwr(f, page, sizeof(page), 0), 4096);
    EwMap m;
    ew_map(&m, f, 1);
    ASSERT_EQ(ew_store(&m, 0, 'a'), MM_FAULT_OK);   /* mapped writable while the volume is */
    ASSERT_EQ(ext2_shutdown(v.sb), 0);
    ASSERT_EQ(ew_store(&m, 1, 'z'), MM_FAULT_OK);   /* dirty again, after the final write-back */
    ASSERT_EQ(filemap_vma_ops.sync(&m.vma, m.vma.start, m.vma.end), -EROFS);
    int requests = blk_mem_requests;
    wb_sync_all();   /* nothing left to try again */
    ASSERT_EQ(blk_mem_requests, requests);
    ASSERT_EQ(vfs_fsync(f), -EROFS);   /* the kept error, once */
    ASSERT_EQ(vfs_fsync(f), 0);
    ew_unmap(&m);
    fput(f);
    ASSERT_EQ(himg_fsck(&v.im), 0);
    ASSERT_EQ(order_kept(), 0);
    ASSERT_TRUE(on_disk_as(&v, "/late", page, sizeof(page)));   /* as shut down */
    return 0;
}

TestCase ext2w_regress_tests[] = {
    TEST_ENTRY(unlinked_device_node_frees_no_blocks),        TEST_ENTRY(stores_past_the_end_never_reach_the_disk),
    TEST_ENTRY(lookup_while_leaving_memory_finds_the_pages), TEST_ENTRY(unlink_gives_back_the_attribute_block),
    TEST_ENTRY(truncate_waits_for_writes_in_flight),         TEST_ENTRY(directory_block_holds_still_while_written),
    TEST_ENTRY(page_dirtied_after_shutdown_is_dropped),
};

int main(int argc, char **argv) {
    vfs_env_arena_size(ARENA_PAGES);
    order_audit_on();
    wb_init();
    reclaim_init();   /* the pressure hook: allocations reclaim clean pages */
    int nf = sizeof(ext2w_files_tests) / sizeof(TestCase), nd = sizeof(ext2w_dirs_tests) / sizeof(TestCase);
    int ns = sizeof(ext2w_space_tests) / sizeof(TestCase), nt = sizeof(ext2w_state_tests) / sizeof(TestCase);
    int nr = sizeof(ext2w_regress_tests) / sizeof(TestCase);
    TestSuite suites[] = {
        {"files", ext2w_files_tests, &nf}, {"dirs", ext2w_dirs_tests, &nd},       {"space", ext2w_space_tests, &ns},
        {"state", ext2w_state_tests, &nt}, {"regress", ext2w_regress_tests, &nr},
    };
    int filtered = 0;
    for (int i = 1; i < argc; i++) filtered |= strcmp(argv[i], "--suite") == 0;
    if (!filtered) {
        printf("test_ext2w_runner: run one suite per process (--suite files|dirs|space|state|regress)\n");
        return 2;
    }
    return test_harness_main(argc, argv, "arc_os ext2 write host tests", suites, 5);
}
