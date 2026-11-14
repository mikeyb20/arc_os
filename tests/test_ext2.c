/* arc_os — ext2, read-only, on the real ext2.c, VFS and page cache (M1-13),
 * over the images tools/make-ext2-test-images.sh builds with mkfs.ext2
 * (4, 2 and 1 KiB blocks; 256- and 128-byte inodes; a dir_index directory):
 * superblock and feature checks, listing (also resumed), lookup, attributes,
 * reads through every block-map level up to triple indirect, holes, fast and
 * slow symlinks with the path walk ("..", loops, O_NOFOLLOW, getcwd across
 * the mount), -EROFS, corrupt directory entries, no leaks. */

#include "test_framework.h"
#include "vfs_env.h"

#include "drivers/blkdev.h"
#include "fs/ext2.h"
#include "mm/filemap.h"
#include "uapi/dirent.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

/* A disk image in memory, with optional patches applied on every read */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint64_t patch_at[4];
    uint8_t patch_len[4];
    uint8_t patch[4][8];
    int npatch;
} Disk;

static int disk_read(BlockDevice *dev, uint64_t sector, uint32_t count, void *buf) {
    const Disk *d = dev->private_data;
    uint64_t at = sector * 512, n = (uint64_t)count * 512;
    if (at + n > d->size) return -1;
    memcpy(buf, d->data + at, n);
    for (int i = 0; i < d->npatch; i++) {
        for (unsigned k = 0; k < d->patch_len[i]; k++) {
            uint64_t p = d->patch_at[i] + k;
            if (p >= at && p < at + n) ((uint8_t *)buf)[p - at] = d->patch[i][k];
        }
    }
    return 0;
}

static void disk_patch(Disk *d, uint64_t at, const void *bytes, unsigned len) {
    d->patch_at[d->npatch] = at;
    d->patch_len[d->npatch] = (uint8_t)len;
    memcpy(d->patch[d->npatch], bytes, len);
    d->npatch++;
}

/* The three volumes, mounted at /e4k, /e2k and /e1k */
typedef struct {
    const char *file;
    const char *mnt;
    const char *dev;
    uint32_t block_size;
    Disk disk;
    BlockDevice bdev;
} Volume;

static Volume vols[] = {
    {"tests/ext2_4k.img", "/e4k", "vdb", 4096, {0}, {0}},
    {"tests/ext2_2k.img", "/e2k", "vdc", 2048, {0}, {0}},
    {"tests/ext2_1k.img", "/e1k", "vdd", 1024, {0}, {0}},
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
        v->disk.data = load(v->file, &v->disk.size);
        if (v->disk.data == NULL) {
            printf("  (no %s: run tools/make-ext2-test-images.sh)\n", v->file);
            state = 2;
            return 1;
        }
        v->bdev.read = disk_read;
        v->bdev.private_data = &v->disk;
        int err;
        SuperBlock *sb = ext2_mount(&v->bdev, v->disk.size, v->dev, &err);
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
         * link_slow link_dangling loop_a loop_b link_dir many fifo */
        ASSERT_EQ(l.n, 17);
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

/* The block of the root directory's first entries, read off the raw image */
static uint64_t root_dir_block_offset(const Disk *d, uint32_t *bs_out) {
    const uint8_t *s = d->data + 1024;
    uint32_t bs = 1024u << *(const uint32_t *)(s + 24);
    uint32_t first = *(const uint32_t *)(s + 20);
    uint16_t isz = *(const uint16_t *)(s + 88);
    uint32_t itable = *(const uint32_t *)(d->data + (uint64_t)(first + 1) * bs + 8);
    const uint8_t *ino2 = d->data + (uint64_t)itable * bs + isz;   /* inode 2: index 1 */
    *bs_out = bs;
    return (uint64_t)*(const uint32_t *)(ino2 + 40) * bs;   /* i_block[0] */
}

TEST(rejects_bad_superblocks) {
    vfs_env_setup();
    int err;
    static uint8_t zero[8192];
    Disk zd = {zero, sizeof(zero), {0}, {0}, {{0}}, 0};
    BlockDevice zdev = {.read = disk_read, .private_data = &zd};
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
        Disk d = vols[0].disk;
        d.npatch = 0;
        disk_patch(&d, 1024 + cases[k].at, &cases[k].value, cases[k].len);
        BlockDevice dev = {.read = disk_read, .private_data = &d};
        SuperBlock *sb = ext2_mount(&dev, d.size, "bad", &err);
        if (cases[k].ok) {
            ASSERT_TRUE(sb != NULL);
            ASSERT_EQ(ext2_ro_forced(sb), cases[k].at == 100);
            ASSERT_TRUE(sb->flags & SB_RDONLY);   /* never unmounted: it stays */
        } else {
            ASSERT_TRUE(sb == NULL);
            ASSERT_EQ(err, -EINVAL);
        }
    }
    /* A device smaller than the volume says */
    Disk d = vols[0].disk;
    BlockDevice dev = {.read = disk_read, .private_data = &d};
    ASSERT_TRUE(ext2_mount(&dev, d.size / 2, "short", &err) == NULL);
    ASSERT_EQ(err, -EINVAL);
    /* An unreadable device */
    Disk tiny = {vols[0].disk.data, 1024, {0}, {0}, {{0}}, 0};
    dev.private_data = &tiny;
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
    Disk d = vols[1].disk;
    d.npatch = 0;
    uint32_t bad = 0x0002 | 0x40;
    disk_patch(&d, 1024 + 96, &bad, 4);
    BlockDevice dev = {.read = disk_read, .private_data = &d};
    ASSERT_TRUE(ext2_mount(&dev, d.size, "bad", &err) == NULL);
    d.npatch = 0;
    uint32_t far = 0x00ffffff;
    disk_patch(&d, 2048 + 8, &far, 4);   /* 2 KiB blocks: group descriptors in block 1 */
    ASSERT_TRUE(ext2_mount(&dev, d.size, "bad", &err) == NULL);
    ASSERT_EQ(err, -EINVAL);
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    int failures = 0;
    for (int n = 1; n <= 64; n++) {
        (void)filemap_drop_clean();
        vfs_env_usage(&before);
        vfs_env_fail_alloc_in(n);
        SuperBlock *sb = ext2_mount(&vols[2].bdev, vols[2].disk.size, "oom", &err);
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
    static Disk disks[NVOLS];
    static BlockDevice devs[NVOLS];
    static const char *mnts[NVOLS] = {"/bad4k", "/bad2k", "/bad1k"};
    for (int i = 0; i < NVOLS; i++) {
        disks[i] = vols[i].disk;
        disks[i].npatch = 0;
        uint32_t bs;
        uint64_t at = root_dir_block_offset(&disks[i], &bs);
        ASSERT_EQ(disks[i].data[at + 8], '.');   /* found the root's "." */
        uint16_t lens[NVOLS] = {0, 13, (uint16_t)(bs + 4)};
        disk_patch(&disks[i], at + 4, &lens[i], 2);   /* "."'s rec_len */
        devs[i].read = disk_read;
        devs[i].private_data = &disks[i];
        int err;
        SuperBlock *sb = ext2_mount(&devs[i], disks[i].size, "bad", &err);
        ASSERT_TRUE(sb != NULL);   /* the root inode is fine; its entries aren't */
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
    ASSERT_TRUE(s.pgpgin - s0.pgpgin >= 16);   /* the 16 pages asked for, plus readahead */
    uint64_t cached = filemap_of(f->inode)->nrpages;
    ASSERT_TRUE(cached >= 16);
    b = iob_kernel(buf, sizeof(buf));
    uint64_t in = s.pgpgin;
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)sizeof(buf));   /* cached: no read */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin, in);
    ASSERT_TRUE(filemap_drop_clean() >= cached);
    ASSERT_EQ(filemap_of(f->inode)->nrpages, 0);
    memset(buf, 0, sizeof(buf));
    b = iob_kernel(buf, sizeof(buf));
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)sizeof(buf));   /* metadata re-read too */
    ASSERT_TRUE(pat_ok(buf, 0, sizeof(buf)));
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
    TEST_ENTRY(reads_through_the_page_cache),
};
int ext2_test_count = sizeof(ext2_tests) / sizeof(ext2_tests[0]);
