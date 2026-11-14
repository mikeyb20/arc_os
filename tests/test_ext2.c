/* arc_os — ext2, read-only, on the real ext2.c, VFS and page cache (M1-13),
 * over the images tools/make-ext2-test-images.sh builds with mkfs.ext2
 * (4, 2 and 1 KiB blocks; 256- and 128-byte inodes; a dir_index directory):
 * superblock and feature checks, listing (also resumed), lookup, attributes,
 * reads through every block-map level up to triple indirect, holes, fast and
 * slow symlinks with the path walk ("..", loops, O_NOFOLLOW, getcwd across
 * the mount), -EROFS, corrupt directory entries, no leaks. */

#include "test_framework.h"
#include "vfs_env.h"
#include "blk_mem.h"

#include "drivers/blkdev.h"
#include "fs/ext2.h"
#include "mm/filemap.h"
#include "mm/pmm.h"
#include "uapi/dirent.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

/* A memory disk (blk_mem.c) over size bytes at data, read-only */
static void disk_init(BlockDevice *dev, uint8_t *data, size_t size) {
    memset(dev, 0, sizeof(*dev));
    dev->driver_data = data;
    dev->nr_sectors = size / 512;
    dev->flags = BLK_RDONLY;
}

/* A copy of an image with len bytes at byte `at` replaced (a corrupt disk) */
static uint8_t *patched(const uint8_t *data, size_t size, uint64_t at, const void *bytes, unsigned len) {
    uint8_t *copy = malloc(size);
    if (copy == NULL) abort();
    memcpy(copy, data, size);
    memcpy(copy + at, bytes, len);
    return copy;
}

/* The three volumes, mounted at /e4k, /e2k and /e1k */
typedef struct {
    const char *file;
    const char *mnt;
    const char *dev;
    uint32_t block_size;
    uint8_t *data;   /* the image */
    size_t size;
    BlockDevice bdev;
} Volume;

static Volume vols[] = {
    {.file = "tests/ext2_4k.img", .mnt = "/e4k", .dev = "vdb", .block_size = 4096},
    {.file = "tests/ext2_2k.img", .mnt = "/e2k", .dev = "vdc", .block_size = 2048},
    {.file = "tests/ext2_1k.img", .mnt = "/e1k", .dev = "vdd", .block_size = 1024},
};
#define NVOLS ((int)(sizeof(vols) / sizeof(vols[0])))

static uint8_t *load(const char *file, size_t *size) {
    char alt[64];
    FILE *f = fopen(file, "rb");
    if (f == NULL) {
        snprintf(alt, sizeof(alt), "../%s", file);
        f = fopen(alt, "rb");
    }
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size);
    if (fread(data, 1, *size, f) != *size) abort();
    fclose(f);
    return data;
}

/* Load and mount all three (once). 0, or 1 to skip. */
static int ext2_ready(void) {
    static int state;   /* 0 = not tried, 1 = mounted, 2 = no images */
    vfs_env_setup();
    if (state != 0) return state == 1 ? 0 : 1;
    for (int i = 0; i < NVOLS; i++) {
        Volume *v = &vols[i];
        v->data = load(v->file, &v->size);
        if (v->data == NULL) {
            printf("  (no %s: run tools/make-ext2-test-images.sh)\n", v->file);
            state = 2;
            return 1;
        }
        disk_init(&v->bdev, v->data, v->size);
        int err;
        SuperBlock *sb = ext2_mount(&v->bdev, v->size, v->dev, &err);
        if (sb == NULL || vfs_mkdir(v->mnt, 0755) != 0 || vfs_mount(v->mnt, sb, v->dev) != 0) abort();
    }
    state = 1;
    return 0;
}

static char pathbuf[256];
static const char *P(const Volume *v, const char *rel) {
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", v->mnt, rel);
    return pathbuf;
}

/* The pattern make-ext2-test-images.sh writes: the byte at file offset i */
static uint8_t pat(uint64_t i) {
    return (uint8_t)((i * 7) ^ (i >> 12) ^ (i >> 20) ^ (i >> 28));
}

static int pat_ok(const uint8_t *buf, uint64_t off, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != pat(off + i)) {
            printf("  pattern differs at file offset %llu\n", (unsigned long long)(off + i));
            return 0;
        }
    }
    return 1;
}

static int zeros(const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != 0) return 0;
    }
    return 1;
}

/* Listing: names, count, and stop after `stop_every` entries per call (0:
 * never) so a listing resumes from the cookie */
typedef struct {
    DirContext ctx;
    int n;
    int taken_this_call;
    int stop_every;
    int has[8];   /* hello.txt, dir, many, fifo, link_fast, sparse.bin, ., .. */
    uint8_t dtypes[8];
    int many_seen[400];
} Listing;

static int listing_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    (void)ino, (void)next;
    Listing *l = (Listing *)ctx;
    if (l->stop_every != 0 && l->taken_this_call == l->stop_every) return 1;
    l->taken_this_call++;
    l->n++;
    static const char *want[8] = {"hello.txt", "dir", "many", "fifo", "link_fast", "sparse.bin", ".", ".."};
    for (int i = 0; i < 8; i++) {
        if (strlen(want[i]) == len && memcmp(want[i], name, len) == 0) {
            l->has[i]++;
            l->dtypes[i] = dtype;
        }
    }
    unsigned k;
    if (len > 5 && memcmp(name, "file_", 5) == 0 && sscanf(name + 5, "%3u", &k) == 1 && k < 400) l->many_seen[k]++;
    return 0;
}

static int list_dir(const char *path, Listing *l, int stop_every) {
    File *d;
    int err = vfs_open(path, O_RDONLY | O_DIRECTORY, 0, &d);
    if (err != 0) return err;
    memset(l, 0, sizeof(*l));
    l->ctx.actor = listing_actor;
    l->stop_every = stop_every;
    for (int calls = 0; calls < 1000; calls++) {
        int before = l->n;
        l->taken_this_call = 0;
        err = vfs_iterate(d, &l->ctx);
        if (err != 0 || l->n == before) break;
    }
    fput(d);
    return err;
}

TEST(mounts_all_geometries) {
    if (ext2_ready() != 0) return 0;
    char m[1024];
    vfs_format_mounts(m, sizeof(m));
    for (int i = 0; i < NVOLS; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%s %s ext2 ro 0 0\n", vols[i].dev, vols[i].mnt);
        ASSERT_TRUE(strstr(m, line) != NULL);
        struct stat st, root;
        ASSERT_EQ(vfs_stat(vols[i].mnt, &st), 0);
        ASSERT_EQ(st.st_ino, 2);   /* EXT2_ROOT_INO */
        ASSERT_EQ(st.st_mode & S_IFMT, S_IFDIR);
        ASSERT_EQ(vfs_stat("/", &root), 0);
        ASSERT_NEQ(st.st_dev, root.st_dev);
        PathLoc loc;
        ASSERT_EQ(vfs_resolve(vols[i].mnt, &loc), 0);
        ASSERT_EQ(ext2_ro_forced(loc.inode->sb), 0);
        ASSERT_TRUE(loc.inode->sb->flags & SB_RDONLY);
        loc_put(&loc);
    }
    return 0;
}

TEST(root_listing) {
    if (ext2_ready() != 0) return 0;
    for (int i = 0; i < NVOLS; i++) {
        Listing l;
        ASSERT_EQ(list_dir(vols[i].mnt, &l, 0), 0);
        /* . .. lost+found hello.txt empty dir medium.bin sparse.bin dddd... link_fast
         * link_slow link_dangling loop_a loop_b link_dir links many fifo */
        ASSERT_EQ(l.n, 18);
        for (int k = 0; k < 8; k++) ASSERT_EQ(l.has[k], 1);
        ASSERT_EQ(l.dtypes[0], DT_REG);   /* the filetype byte */
        ASSERT_EQ(l.dtypes[1], DT_DIR);
        ASSERT_EQ(l.dtypes[3], DT_FIFO);
        ASSERT_EQ(l.dtypes[4], DT_LNK);
        /* getdents and stat agree on inode numbers */
        struct stat st;
        ASSERT_EQ(vfs_stat(P(&vols[i], "hello.txt"), &st), 0);
        ASSERT_TRUE(st.st_ino > 2);
    }
    return 0;
}

/* A dir_index (htree) directory of 400 entries over several blocks: read
 * linearly, also in small resumed batches */
TEST(indexed_directory) {
    if (ext2_ready() != 0) return 0;
    for (int i = 0; i < NVOLS; i++) {
        int batches[] = {0, 1, 37};
        for (int b = 0; b < 3; b++) {
            Listing l;
            ASSERT_EQ(list_dir(P(&vols[i], "many"), &l, batches[b]), 0);
            ASSERT_EQ(l.n, 402);
            ASSERT_EQ(l.has[6], 1);
            ASSERT_EQ(l.has[7], 1);
            for (int k = 0; k < 400; k++) ASSERT_EQ(l.many_seen[k], 1);
        }
        char text[16];
        ASSERT_EQ(vfs_env_read_file(P(&vols[i], "many/file_399_with_a_longish_name"), text, sizeof(text)), 4);
        ASSERT_STR_EQ(text, "399\n");
        ASSERT_EQ(vfs_env_read_file(P(&vols[i], "many/file_000_with_a_longish_name"), text, sizeof(text)), 2);
        struct stat st;
        ASSERT_EQ(vfs_stat(P(&vols[i], "many/file_400_with_a_longish_name"), &st), -ENOENT);
        ASSERT_EQ(vfs_stat(P(&vols[i], "many/file_00"), &st), -ENOENT);   /* a prefix is no match */
    }
    return 0;
}

TEST(attributes) {
    if (ext2_ready() != 0) return 0;
    for (int i = 0; i < NVOLS; i++) {
        const Volume *v = &vols[i];
        struct stat st;
        ASSERT_EQ(vfs_stat(P(v, "hello.txt"), &st), 0);
        ASSERT_EQ(st.st_mode, S_IFREG | 0640);
        ASSERT_EQ(st.st_size, 13);
        ASSERT_EQ(st.st_nlink, 1);
        ASSERT_EQ(st.st_blocks, v->block_size / 512);
        ASSERT_TRUE(st.st_mtime > 1700000000);   /* the images are recent */
        ASSERT_EQ(vfs_stat(P(v, "empty"), &st), 0);
        ASSERT_EQ(st.st_size, 0);
        ASSERT_EQ(st.st_blocks, 0);
        ASSERT_EQ(vfs_stat(P(v, "dir"), &st), 0);
        ASSERT_EQ(st.st_mode & S_IFMT, S_IFDIR);
        ASSERT_EQ(st.st_nlink, 3);   /* ".", its entry, a's ".." */
        ASSERT_EQ(vfs_stat(P(v, "fifo"), &st), 0);
        ASSERT_EQ(st.st_mode & S_IFMT, S_IFIFO);
        ASSERT_EQ(vfs_stat(P(v, "sparse.bin"), &st), 0);
        ASSERT_EQ(st.st_size, 5ll << 30);
        ASSERT_TRUE(st.st_blocks < 256);   /* holes take no blocks */
        File *f;
        ASSERT_EQ(vfs_open(P(v, "fifo"), O_RDONLY | O_NONBLOCK, 0, &f), -ENXIO);
    }
    return 0;
}

TEST(reads_small_files) {
    if (ext2_ready() != 0) return 0;
    for (int i = 0; i < NVOLS; i++) {
        const Volume *v = &vols[i];
        char text[64];
        ASSERT_EQ(vfs_env_read_file(P(v, "hello.txt"), text, sizeof(text)), 13);
        ASSERT_STR_EQ(text, "Hello, ext2!\n");
        ASSERT_EQ(vfs_env_read_file(P(v, "dir/a/b/c/deep.txt"), text, sizeof(text)), 5);
        ASSERT_STR_EQ(text, "deep\n");
        ASSERT_EQ(vfs_env_read_file(P(v, "empty"), text, sizeof(text)), 0);
        File *f;
        ASSERT_EQ(vfs_open(P(v, "hello.txt"), O_RDONLY, 0, &f), 0);
        IoBuf b = iob_kernel(text, 4);
        ASSERT_EQ(vfs_pread(f, &b, 7), 4);
        ASSERT_MEM_EQ(text, "ext2", 4);
        ASSERT_EQ(vfs_pread(f, &b, 13), 0);
        ASSERT_EQ(vfs_pread(f, &b, 1ll << 40), 0);
        b = iob_user(VFS_ENV_BAD_UADDR, 4);
        ASSERT_EQ(vfs_read(f, &b), -EFAULT);
        fput(f);
        /* The bytes past the end in the last cached page are zeros */
        unsigned off;
        ASSERT_EQ(vfs_open(P(v, "hello.txt"), O_RDONLY, 0, &f), 0);
        struct page *pg = filemap_find(filemap_of(f->inode), 0, &off);
        ASSERT_TRUE(pg != NULL);
        ASSERT_TRUE(zeros((uint8_t *)page_to_virt(pg) + 13, 4096 - 13));
        page_put(pg);
        fput(f);
    }
    return 0;
}

/* 300000 bytes: direct and single-indirect blocks, double indirect with
 * 1 KiB blocks; read whole, then in odd-sized pieces at odd offsets */
TEST(reads_medium_file) {
    if (ext2_ready() != 0) return 0;
    static uint8_t buf[300000 + 100];
    for (int i = 0; i < NVOLS; i++) {
        (void)filemap_drop_clean();
        ASSERT_EQ(vfs_env_read_file(P(&vols[i], "medium.bin"), (char *)buf, sizeof(buf)), 300000);
        ASSERT_TRUE(pat_ok(buf, 0, 300000));
        File *f;
        ASSERT_EQ(vfs_open(P(&vols[i], "medium.bin"), O_RDONLY, 0, &f), 0);
        (void)filemap_drop_clean();
        loff_t offs[] = {299999, 12 * 1024 - 3, 268 * 1024 - 1, 49151, 4095, 0, 150001};
        for (size_t k = 0; k < sizeof(offs) / sizeof(offs[0]); k++) {
            IoBuf b = iob_kernel(buf, 9000);
            int64_t n = vfs_pread(f, &b, offs[k]);
            size_t want = 300000 - (size_t)offs[k] < 9000 ? 300000 - (size_t)offs[k] : 9000;
            ASSERT_EQ(n, (int64_t)want);
            ASSERT_TRUE(pat_ok(buf, (uint64_t)offs[k], want));
        }
        fput(f);
    }
    return 0;
}

/* A 5 GiB file with data at each block-map level (the last just under the
 * end: triple indirect at every block size) and holes between */
TEST(reads_sparse_file_to_triple_indirect) {
    if (ext2_ready() != 0) return 0;
    static uint8_t buf[8192];
    const uint64_t data[] = {0, 49152, 4243456, 1ull << 30, (5ull << 30) - 4096};
    const uint64_t holes[] = {8192, 2u << 20, 3ull << 30, (5ull << 30) - 8192};
    for (int i = 0; i < NVOLS; i++) {
        File *f;
        ASSERT_EQ(vfs_open(P(&vols[i], "sparse.bin"), O_RDONLY, 0, &f), 0);
        for (size_t k = 0; k < sizeof(data) / sizeof(data[0]); k++) {
            (void)filemap_drop_clean();
            IoBuf b = iob_kernel(buf, 4096);
            ASSERT_EQ(vfs_pread(f, &b, (loff_t)data[k]), 4096);
            ASSERT_TRUE(pat_ok(buf, data[k], 4096));
        }
        for (size_t k = 0; k < sizeof(holes) / sizeof(holes[0]); k++) {
            (void)filemap_drop_clean();
            memset(buf, 0xAA, sizeof(buf));
            IoBuf b = iob_kernel(buf, 4096);
            ASSERT_EQ(vfs_pread(f, &b, (loff_t)holes[k]), 4096);
            ASSERT_TRUE(zeros(buf, 4096));
        }
        /* Across the boundary between a hole and data, and the end */
        IoBuf b = iob_kernel(buf, 8192);
        ASSERT_EQ(vfs_pread(f, &b, (loff_t)((5ull << 30) - 8192)), 8192);
        ASSERT_TRUE(zeros(buf, 4096));
        ASSERT_TRUE(pat_ok(buf + 4096, (5ull << 30) - 4096, 4096));
        ASSERT_EQ(vfs_pread(f, &b, (loff_t)(5ull << 30) - 100), 100);
        ASSERT_EQ(vfs_pread(f, &b, (loff_t)(5ull << 30)), 0);
        fput(f);
        (void)filemap_drop_clean();
    }
    return 0;
}

TEST(symlinks) {
    if (ext2_ready() != 0) return 0;
    for (int i = 0; i < NVOLS; i++) {
        const Volume *v = &vols[i];
        char text[128];
        ASSERT_EQ(vfs_env_read_file(P(v, "link_fast"), text, sizeof(text)), 13);   /* target in the inode */
        ASSERT_STR_EQ(text, "Hello, ext2!\n");
        ASSERT_EQ(vfs_env_read_file(P(v, "link_slow"), text, sizeof(text)), 12);   /* target in a block */
        ASSERT_STR_EQ(text, "slow target\n");
        ASSERT_EQ(vfs_env_read_file(P(v, "dir/a/up"), text, sizeof(text)), 13);   /* "../../hello.txt" */
        ASSERT_EQ(vfs_env_read_file(P(v, "link_dir/b/c/deep.txt"), text, sizeof(text)), 5);
        ASSERT_EQ(vfs_readlink_at(NULL, P(v, "link_fast"), text, sizeof(text)), 9);
        ASSERT_MEM_EQ(text, "hello.txt", 9);
        ASSERT_EQ(vfs_readlink_at(NULL, P(v, "link_slow"), text, sizeof(text)), 92);
        ASSERT_MEM_EQ(text, "dddddddddd", 10);
        ASSERT_MEM_EQ(text + 81, "/target.txt", 11);
        ASSERT_EQ(vfs_readlink_at(NULL, P(v, "link_slow"), text, 5), 5);   /* cut short */
        ASSERT_EQ(vfs_readlink_at(NULL, P(v, "hello.txt"), text, sizeof(text)), -EINVAL);
        struct stat st;
        ASSERT_EQ(vfs_stat_at(NULL, P(v, "link_slow"), 0, &st), 0);   /* lstat */
        ASSERT_EQ(st.st_mode, S_IFLNK | 0777);
        ASSERT_EQ(st.st_size, 92);
        ASSERT_EQ(vfs_stat(P(v, "link_dangling"), &st), -ENOENT);
        ASSERT_EQ(vfs_stat_at(NULL, P(v, "link_dangling"), 0, &st), 0);
        ASSERT_EQ(vfs_stat(P(v, "loop_a"), &st), -ELOOP);
        File *f;
        /* Exactly 40 links resolve; 41 are -ELOOP (Linux's limit) */
        ASSERT_EQ(vfs_stat(P(v, "links/l00"), &st), 0);
        ASSERT_EQ(st.st_size, 13);   /* hello.txt */
        ASSERT_EQ(vfs_env_read_file(P(v, "links/l00"), text, sizeof(text)), 13);
        ASSERT_EQ(vfs_stat(P(v, "links/over"), &st), -ELOOP);
        ASSERT_EQ(vfs_open(P(v, "links/over"), O_RDONLY, 0, &f), -ELOOP);
        ASSERT_EQ(vfs_open(P(v, "link_fast"), O_RDONLY | O_NOFOLLOW, 0, &f), -ELOOP);
        ASSERT_EQ(vfs_open(P(v, "link_dir/"), O_RDONLY | O_DIRECTORY, 0, &f), 0);   /* trailing '/' follows */
        fput(f);
    }
    return 0;
}

/* ".." inside the volume uses its own entries; at the volume's root it
 * crosses back to the mount point's parent */
TEST(dotdot_and_cwd) {
    if (ext2_ready() != 0) return 0;
    for (int i = 0; i < NVOLS; i++) {
        const Volume *v = &vols[i];
        struct stat a, b;
        ASSERT_EQ(vfs_stat(P(v, "dir/a/b/../../../hello.txt"), &a), 0);
        ASSERT_EQ(vfs_stat(P(v, "hello.txt"), &b), 0);
        ASSERT_EQ(a.st_ino, b.st_ino);
        ASSERT_EQ(vfs_stat(P(v, ".."), &a), 0);
        ASSERT_EQ(vfs_stat("/", &b), 0);
        ASSERT_EQ(a.st_ino, b.st_ino);
        ASSERT_EQ(a.st_dev, b.st_dev);
        PathLoc loc;
        ASSERT_EQ(vfs_resolve(P(v, "dir/a/b/c"), &loc), 0);
        char path[128];
        int n = vfs_path_of(&loc, path, sizeof(path));
        snprintf(pathbuf, sizeof(pathbuf), "%s/dir/a/b/c", v->mnt);
        ASSERT_EQ(n, (int)strlen(pathbuf));
        ASSERT_STR_EQ(path, pathbuf);
        vfs_cwd_set(&loc);   /* relative paths from inside the volume (as the syscalls pass them) */
        PathLoc cwd;
        vfs_cwd_get(&cwd);
        ASSERT_EQ(vfs_stat_at(&cwd, "deep.txt", WALK_FOLLOW, &a), 0);
        ASSERT_EQ(vfs_stat_at(&cwd, "../../../../hello.txt", WALK_FOLLOW, &b), 0);
        ASSERT_EQ(b.st_size, 13);
        ASSERT_EQ(vfs_stat_at(&cwd, "../../../../../e4k/hello.txt", WALK_FOLLOW, &b), 0);
        n = vfs_path_of(&cwd, path, sizeof(path));
        ASSERT_STR_EQ(path, pathbuf);   /* getcwd */
        loc_put(&cwd);
        ASSERT_EQ(vfs_resolve("/", &loc), 0);
        vfs_cwd_set(&loc);
    }
    return 0;
}

TEST(changes_get_erofs) {
    if (ext2_ready() != 0) return 0;
    const Volume *v = &vols[0];
    File *f;
    ASSERT_EQ(vfs_open(P(v, "hello.txt"), O_WRONLY, 0, &f), -EROFS);
    ASSERT_EQ(vfs_open(P(v, "hello.txt"), O_RDWR, 0, &f), -EROFS);
    ASSERT_EQ(vfs_open(P(v, "hello.txt"), O_RDONLY | O_TRUNC, 0, &f), -EROFS);
    ASSERT_EQ(vfs_open(P(v, "new.txt"), O_CREAT | O_WRONLY, 0644, &f), -EROFS);
    ASSERT_EQ(vfs_open(P(v, "link_dangling"), O_CREAT | O_WRONLY, 0644, &f), -EROFS);
    ASSERT_EQ(vfs_mkdir(P(v, "newdir"), 0755), -EROFS);
    ASSERT_EQ(vfs_unlink(P(v, "hello.txt")), -EROFS);
    ASSERT_EQ(vfs_rmdir(P(v, "dir")), -EROFS);
    char other[64];
    snprintf(other, sizeof(other), "%s/bye.txt", v->mnt);
    ASSERT_EQ(vfs_rename(P(v, "hello.txt"), other), -EROFS);
    PathLoc loc;
    ASSERT_EQ(vfs_resolve(P(v, "hello.txt"), &loc), 0);
    InodeAttr a = {.valid = ATTR_MODE, .mode = 0600};
    ASSERT_EQ(vfs_setattr(loc.inode, &a), -EROFS);
    loc_put(&loc);
    return 0;
}

TEST(inodes_are_shared_and_leak_free) {
    if (ext2_ready() != 0) return 0;
    File *a, *b;
    ASSERT_EQ(vfs_open("/e2k/hello.txt", O_RDONLY, 0, &a), 0);
    ASSERT_EQ(vfs_open("/e2k/link_fast", O_RDONLY, 0, &b), 0);
    ASSERT_TRUE(a->inode == b->inode);   /* one inode per number (inode cache) */
    fput(a);
    fput(b);
    char text[64];
    for (int warm = 0; warm < 2; warm++) {
        VfsEnvUsage before, after;
        vfs_env_usage(&before);
        for (int i = 0; i < 5; i++) {
            ASSERT_TRUE(vfs_env_read_file("/e1k/link_slow", text, sizeof(text)) > 0);
            ASSERT_TRUE(vfs_env_read_file("/e1k/dir/a/up", text, sizeof(text)) > 0);
            Listing l;
            ASSERT_EQ(list_dir("/e1k/dir/a", &l, 0), 0);
        }
        vfs_env_usage(&after);
        if (warm == 1) ASSERT_TRUE(vfs_env_no_leak(&before, &after));   /* the first round fills the caches */
    }
    return 0;
}

/* Byte offset of the first block of directory inode ino, read off the raw
 * image (one-group lookup through the group descriptor table) */
static uint64_t dir_block_offset(const uint8_t *data, uint32_t ino, uint32_t *bs_out) {
    const uint8_t *s = data + 1024;
    uint32_t bs = 1024u << *(const uint32_t *)(s + 24);
    uint32_t first = *(const uint32_t *)(s + 20);
    uint32_t per_group = *(const uint32_t *)(s + 40);
    uint16_t isz = *(const uint16_t *)(s + 88);
    uint32_t group = (ino - 1) / per_group, index = (ino - 1) % per_group;
    uint32_t itable = *(const uint32_t *)(data + (uint64_t)(first + 1) * bs + group * 32 + 8);
    const uint8_t *inode = data + (uint64_t)itable * bs + (uint64_t)index * isz;
    *bs_out = bs;
    uint32_t block0 = *(const uint32_t *)(inode + 40);   /* i_block[0] */
    return (uint64_t)block0 * bs;
}

/* The block of the root directory's first entries */
static uint64_t root_dir_block_offset(const uint8_t *data, uint32_t *bs_out) {
    return dir_block_offset(data, 2, bs_out);
}

TEST(rejects_bad_superblocks) {
    vfs_env_setup();
    int err;
    static uint8_t zero[8192];
    BlockDevice zdev;
    disk_init(&zdev, zero, sizeof(zero));
    ASSERT_TRUE(ext2_mount(&zdev, sizeof(zero), "z", &err) == NULL);   /* no magic */
    ASSERT_EQ(err, -EINVAL);
    ASSERT_TRUE(ext2_mount(NULL, 1 << 20, "z", &err) == NULL);
    if (ext2_ready() != 0) return 0;
    struct {
        uint32_t at;   /* byte in the superblock */
        uint32_t value;
        unsigned len;
        int ok;   /* 1: mounts anyway */
    } cases[] = {
        {56, 0x1234, 2, 0},       /* s_magic */
        {76, 0, 4, 0},            /* s_rev_level 0 */
        {24, 3, 4, 0},            /* s_log_block_size: 8 KiB */
        {88, 512, 2, 0},          /* s_inode_size */
        {96, 0x0002 | 0x40, 4, 0},  /* s_feature_incompat: filetype + extents */
        {96, 0x0002 | 0x04, 4, 0},  /* ... + journal recovery needed */
        {4, 0x7fffffff, 4, 0},    /* s_blocks_count past the device */
        {32, 0, 4, 0},            /* s_blocks_per_group 0 */
        {40, 0, 4, 0},            /* s_inodes_per_group 0 */
        {100, 0x0003 | 0x0400, 4, 1},  /* s_feature_ro_compat + metadata_csum: read-only for good */
        {92, 0x0020 | 0x0004, 4, 1},   /* s_feature_compat + has_journal: ignored */
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        uint8_t *copy = patched(vols[0].data, vols[0].size, 1024 + cases[k].at, &cases[k].value, cases[k].len);
        BlockDevice *dev = malloc(sizeof(*dev));   /* a mounted volume keeps both */
        disk_init(dev, copy, vols[0].size);
        SuperBlock *sb = ext2_mount(dev, vols[0].size, "bad", &err);
        if (cases[k].ok) {
            ASSERT_TRUE(sb != NULL);
            ASSERT_EQ(ext2_ro_forced(sb), cases[k].at == 100);
            ASSERT_TRUE(sb->flags & SB_RDONLY);   /* never unmounted: it stays */
        } else {
            ASSERT_TRUE(sb == NULL);
            ASSERT_EQ(err, -EINVAL);
            free(dev);
            free(copy);
        }
    }
    /* A device smaller than the volume says */
    BlockDevice dev;
    disk_init(&dev, vols[0].data, vols[0].size);
    ASSERT_TRUE(ext2_mount(&dev, vols[0].size / 2, "short", &err) == NULL);
    ASSERT_EQ(err, -EINVAL);
    /* An unreadable device: two sectors, the superblock past them */
    disk_init(&dev, vols[0].data, 1024);
    ASSERT_TRUE(ext2_mount(&dev, 1 << 20, "eio", &err) == NULL);
    ASSERT_EQ(err, -EIO);
    return 0;
}

/* Failed mounts free everything they took: refused features, a bad group
 * descriptor, and running out of memory at each allocation in turn */
TEST(failed_mounts_leak_nothing) {
    if (ext2_ready() != 0) return 0;
    int err;
    VfsEnvUsage before, after;
    (void)filemap_drop_clean();
    vfs_env_usage(&before);
    uint32_t bad = 0x0002 | 0x40;
    uint8_t *copy = patched(vols[1].data, vols[1].size, 1024 + 96, &bad, 4);
    BlockDevice dev;
    disk_init(&dev, copy, vols[1].size);
    ASSERT_TRUE(ext2_mount(&dev, vols[1].size, "bad", &err) == NULL);
    free(copy);
    uint32_t far = 0x00ffffff;
    copy = patched(vols[1].data, vols[1].size, 2048 + 8, &far, 4);   /* 2 KiB blocks: group descriptors in block 1 */
    disk_init(&dev, copy, vols[1].size);
    ASSERT_TRUE(ext2_mount(&dev, vols[1].size, "bad", &err) == NULL);
    ASSERT_EQ(err, -EINVAL);
    free(copy);
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    int failures = 0;
    for (int n = 1; n <= 64; n++) {
        (void)filemap_drop_clean();
        vfs_env_usage(&before);
        vfs_env_fail_alloc_in(n);
        SuperBlock *sb = ext2_mount(&vols[2].bdev, vols[2].size, "oom", &err);
        vfs_env_fail_alloc_in(0);
        if (sb != NULL) break;   /* past the last allocation (this volume stays) */
        failures++;
        ASSERT_TRUE(err == -ENOMEM || err == -EIO);
        (void)filemap_drop_clean();
        vfs_env_usage(&after);
        ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    }
    ASSERT_TRUE(failures >= 4);
    return 0;
}

/* A directory entry whose rec_len is zero, not a multiple of 4, or runs
 * off the block: -EIO (logged), never a read past it. One kind per geometry
 * (mount table slots are few). */
TEST(corrupt_directory_entry_is_eio) {
    if (ext2_ready() != 0) return 0;
    static BlockDevice devs[NVOLS];   /* the volumes stay mounted */
    static const char *mnts[NVOLS] = {"/bad4k", "/bad2k", "/bad1k"};
    for (int i = 0; i < NVOLS; i++) {
        uint32_t bs;
        uint64_t at = root_dir_block_offset(vols[i].data, &bs);
        ASSERT_EQ(vols[i].data[at + 8], '.');   /* found the root's "." */
        uint16_t lens[NVOLS] = {0, 13, (uint16_t)(bs + 4)};
        disk_init(&devs[i], patched(vols[i].data, vols[i].size, at + 4, &lens[i], 2), vols[i].size); /* "."'s rec_len */
        int err;
        SuperBlock *sb = ext2_mount(&devs[i], vols[i].size, "bad", &err);
        ASSERT_TRUE(sb != NULL); /* the root inode is fine; its entries aren't */
        ASSERT_EQ(vfs_mkdir(mnts[i], 0755), 0);
        ASSERT_EQ(vfs_mount(mnts[i], sb, "bad"), 0);
        Listing l;
        ASSERT_EQ(list_dir(mnts[i], &l, 0), -EIO);
        struct stat st;
        snprintf(pathbuf, sizeof(pathbuf), "%s/hello.txt", mnts[i]);
        ASSERT_EQ(vfs_stat(pathbuf, &st), -EIO);
    }
    return 0;
}

/* A corrupt ".." (only a damaged disk has one): naming a regular file, or
 * the volume's root from below a subtree mount's root, is -EIO, never a walk
 * to that inode or out of the subtree. "dir" mounted alone, as /apps is. */
TEST(corrupt_dotdot_is_eio) {
    if (ext2_ready() != 0) return 0;
    struct stat a, b, file;
    ASSERT_EQ(vfs_stat("/e4k/dir/a", &a), 0);
    ASSERT_EQ(vfs_stat("/e4k/dir/a/b", &b), 0);
    ASSERT_EQ(vfs_stat("/e4k/hello.txt", &file), 0);
    uint8_t *copy = malloc(vols[0].size);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, vols[0].data, vols[0].size);
    uint32_t bs;
    uint64_t at = dir_block_offset(copy, (uint32_t)a.st_ino, &bs) + 12; /* past "." */
    ASSERT_MEM_EQ(copy + at + 8, "..", 2);
    uint32_t ino = (uint32_t)file.st_ino;
    memcpy(copy + at, &ino, 4); /* dir/a/.. -> hello.txt */
    at = dir_block_offset(copy, (uint32_t)b.st_ino, &bs) + 12;
    ASSERT_MEM_EQ(copy + at + 8, "..", 2);
    ino = 2;
    memcpy(copy + at, &ino, 4); /* dir/a/b/.. -> the volume's root */
    static BlockDevice dev; /* stays mounted */
    disk_init(&dev, copy, vols[0].size);
    int err;
    SuperBlock *sb = ext2_mount(&dev, vols[0].size, "dd", &err);
    ASSERT_TRUE(sb != NULL);
    ASSERT_EQ(vfs_mkdir("/dsub", 0755), 0);
    ASSERT_EQ(vfs_mount_subdir("/dsub", sb, "dir", "dd"), 0);
    struct stat st;
    ASSERT_EQ(vfs_stat("/dsub/a/b/c/deep.txt", &st), 0); /* the rest is fine */
    ASSERT_EQ(vfs_stat("/dsub/a/..", &st), -EIO);
    ASSERT_EQ(vfs_stat("/dsub/a/b/../hello.txt", &st), -EIO);
    ASSERT_EQ(vfs_stat("/dsub/a/b/..", &st), -EIO);
    struct stat root;
    ASSERT_EQ(vfs_stat("/", &root), 0);
    ASSERT_EQ(vfs_stat("/dsub/..", &st), 0); /* the mount's own root still steps out */
    ASSERT_EQ(st.st_ino, root.st_ino);
    return 0;
}

/* File data goes through the page cache: drop_caches frees it and the next
 * read fills it again; the metadata pages of the volume can be dropped too */
TEST(reads_through_the_page_cache) {
    if (ext2_ready() != 0) return 0;
    static uint8_t buf[65536];
    File *f;
    ASSERT_EQ(vfs_open("/e4k/medium.bin", O_RDONLY, 0, &f), 0);
    (void)filemap_drop_clean();
    FilemapStats s0, s;
    filemap_stats(&s0);
    IoBuf b = iob_kernel(buf, sizeof(buf));
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)sizeof(buf));
    ASSERT_TRUE(pat_ok(buf, 0, sizeof(buf)));
    filemap_stats(&s);
    ASSERT_TRUE(s.pgpgin - s0.pgpgin >= 16); /* the 16 pages asked for, plus readahead */
    uint64_t cached = filemap_of(f->inode)->nrpages;
    ASSERT_TRUE(cached >= 16);
    b = iob_kernel(buf, sizeof(buf));
    uint64_t in = s.pgpgin;
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)sizeof(buf)); /* cached: no read */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin, in);
    ASSERT_TRUE(filemap_drop_clean() >= cached);
    ASSERT_EQ(filemap_of(f->inode)->nrpages, 0);
    memset(buf, 0, sizeof(buf));
    b = iob_kernel(buf, sizeof(buf));
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)sizeof(buf)); /* metadata re-read too */
    ASSERT_TRUE(pat_ok(buf, 0, sizeof(buf)));
    fput(f);
    return 0;
}

/* A readahead batch with async pieces (deferred reads write BLK_MEM_POISON):
 * a whole page's blocks go on the plug and land late; the page holding EOF is
 * copied from the metadata cache with its tail zeroed, never touched by a
 * piece, so it is done (and right) before any interrupt. 1 KiB blocks. */
TEST(readahead_pieces_land_late_eof_page_does_not_wait) {
    if (ext2_ready() != 0) return 0;
    File *f;
    ASSERT_EQ(vfs_open("/e1k/medium.bin", O_RDONLY, 0, &f), 0);
    AddressSpace *as = f->inode->mapping;
    uint64_t last = (300000 - 1) / PAGE_SIZE;
    size_t eof = 300000 - last * PAGE_SIZE;
    ASSERT_TRUE(eof < 1024); /* EOF in the page's first block: no piece */
    struct page *pg[2];
    for (int i = 0; i < 2; i++) {
        pg[i] = alloc_pages(0, 0);
        ASSERT_TRUE(pg[i] != NULL);
        pg[i]->index = last - 1 + (uint64_t)i;
    }
    /* Warm the metadata cache (block map, the EOF block) with a plain read */
    page_set(pg[1], PG_LOCKED);
    ASSERT_EQ(as->ops->readpage(as, pg[1]), 0);
    ASSERT_TRUE(!page_test(pg[1], PG_LOCKED) && page_test(pg[1], PG_UPTODATE));
    page_clear(pg[1], PG_UPTODATE);
    for (int i = 0; i < 2; i++) page_set(pg[i], PG_LOCKED); /* as readahead hands them over */
    int before = blk_mem_requests;
    blk_mem_defer = 1;
    as->ops->readpages(as, pg, 2);
    blk_mem_defer = 0;
    ASSERT_TRUE(page_test(pg[0], PG_LOCKED)); /* its pieces are still in flight */
    ASSERT_TRUE(!page_test(pg[1], PG_LOCKED) && page_test(pg[1], PG_UPTODATE));
    ASSERT_TRUE(blk_mem_requests == before); /* nothing reached the disk yet */
    blk_mem_run();
    ASSERT_TRUE(blk_mem_requests > before);
    ASSERT_TRUE(!page_test(pg[0], PG_LOCKED) && page_test(pg[0], PG_UPTODATE));
    const uint8_t *va = page_to_virt(pg[0]);
    for (size_t i = 0; i < PAGE_SIZE; i++) ASSERT_EQ(va[i], BLK_MEM_POISON);
    va = page_to_virt(pg[1]);
    ASSERT_TRUE(pat_ok(va, last * PAGE_SIZE, eof));
    ASSERT_TRUE(zeros(va + eof, PAGE_SIZE - eof));
    for (int i = 0; i < 2; i++) {
        page_clear(pg[i], PG_UPTODATE);
        free_pages(pg[i], 0);
    }
    fput(f);
    return 0;
}

TestCase ext2_tests[] = {
    TEST_ENTRY(mounts_all_geometries),
    TEST_ENTRY(root_listing),
    TEST_ENTRY(indexed_directory),
    TEST_ENTRY(attributes),
    TEST_ENTRY(reads_small_files),
    TEST_ENTRY(reads_medium_file),
    TEST_ENTRY(reads_sparse_file_to_triple_indirect),
    TEST_ENTRY(symlinks),
    TEST_ENTRY(dotdot_and_cwd),
    TEST_ENTRY(changes_get_erofs),
    TEST_ENTRY(inodes_are_shared_and_leak_free),
    TEST_ENTRY(rejects_bad_superblocks),
    TEST_ENTRY(failed_mounts_leak_nothing),
    TEST_ENTRY(corrupt_directory_entry_is_eio),
    TEST_ENTRY(corrupt_dotdot_is_eio),
    TEST_ENTRY(reads_through_the_page_cache),
    TEST_ENTRY(readahead_pieces_land_late_eof_page_does_not_wait),
};
int ext2_test_count = sizeof(ext2_tests) / sizeof(ext2_tests[0]);
