/* arc_os — FAT32, read-only, on the real fat32.c and VFS (M1-11), over the
 * image tools/make-fat32-test-disk.sh builds: boot-sector checks, listing,
 * 8.3 lookup in any case, reads across clusters, stable inode numbers, and
 * -EROFS for every change. */

#include "test_framework.h"
#include "vfs_env.h"

#include "drivers/blkdev.h"
#include "fs/fat32.h"
#include "mm/filemap.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

static uint8_t *image;
static size_t image_size;

static int mem_read(BlockDevice *dev, uint64_t sector, uint32_t count, void *buf) {
    const uint8_t *base = dev->private_data;
    size_t size = dev == NULL ? 0 : (base == image ? image_size : 512);
    if ((sector + count) * 512 > size) return -1;
    memcpy(buf, base + sector * 512, (size_t)count * 512);
    return 0;
}

static BlockDevice disk = {.read = mem_read};

/* Load the image and mount it at /fat (once). 0, or 1 to skip. */
static int fat_ready(void) {
    static int state;   /* 0 = not tried, 1 = mounted, 2 = no image */
    vfs_env_setup();
    if (state != 0) return state == 1 ? 0 : 1;
    FILE *f = fopen("tests/fat32_test.img", "rb");
    if (f == NULL) f = fopen("../tests/fat32_test.img", "rb");
    if (f == NULL) {
        printf("  (no tests/fat32_test.img: run tools/make-fat32-test-disk.sh)\n");
        state = 2;
        return 1;
    }
    fseek(f, 0, SEEK_END);
    image_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    image = malloc(image_size);
    if (fread(image, 1, image_size, f) != image_size) abort();
    fclose(f);
    disk.private_data = image;
    SuperBlock *sb = fat32_mount(&disk);
    if (sb == NULL || vfs_mkdir("/fat", 0755) != 0 || vfs_mount("/fat", sb, "vda") != 0) abort();
    state = 1;
    return 0;
}

static char text[16384];

typedef struct {
    DirContext ctx;
    char names[256];
    uint64_t inos[16];
    int n;
} Names;

static int names_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    (void)dtype, (void)next;
    Names *l = (Names *)ctx;
    size_t at = strlen(l->names);
    snprintf(l->names + at, sizeof(l->names) - at, "%s%.*s", at ? " " : "", (int)len, name);
    if (l->n < 16) l->inos[l->n++] = ino;
    return 0;
}

static int list(const char *path, Names *l) {
    File *d;
    int err = vfs_open(path, O_RDONLY | O_DIRECTORY, 0, &d);
    if (err != 0) return err;
    memset(l, 0, sizeof(*l));
    l->ctx.actor = names_actor;
    err = vfs_iterate(d, &l->ctx);
    fput(d);
    return err;
}

TEST(rejects_bad_boot_sectors) {
    vfs_env_setup();
    static uint8_t sector[512];
    BlockDevice bad = {.read = mem_read, .private_data = sector};
    ASSERT_TRUE(fat32_mount(&bad) == NULL);   /* all zeros */
    if (fat_ready() != 0) return 0;
    memcpy(sector, image, 512);
    Fat32Bpb *bpb = (Fat32Bpb *)sector;
    bpb->bytes_per_sector = 1024;
    ASSERT_TRUE(fat32_mount(&bad) == NULL);
    memcpy(sector, image, 512);
    bpb->root_entry_count = 512;   /* FAT12/16 */
    ASSERT_TRUE(fat32_mount(&bad) == NULL);
    memcpy(sector, image, 512);
    bpb->total_sectors_32 = 4;   /* data region past the end */
    ASSERT_TRUE(fat32_mount(&bad) == NULL);
    ASSERT_TRUE(fat32_mount(NULL) == NULL);
    return 0;
}

TEST(mounted_read_only) {
    if (fat_ready() != 0) return 0;
    char m[1024];
    vfs_format_mounts(m, sizeof(m));
    ASSERT_TRUE(strstr(m, "vda /fat vfat ro 0 0\n") != NULL);
    return 0;
}

TEST(root_listing) {
    if (fat_ready() != 0) return 0;
    Names l;
    ASSERT_EQ(list("/fat", &l), 0);
    ASSERT_STR_EQ(l.names, ". .. subdir hello.txt big.txt ro.txt");
    struct stat st;
    ASSERT_EQ(vfs_stat("/fat/hello.txt", &st), 0);
    ASSERT_EQ(l.inos[3], st.st_ino);   /* getdents and stat agree */
    ASSERT_EQ(list("/fat/subdir", &l), 0);
    ASSERT_STR_EQ(l.names, ". .. nested.txt");
    return 0;
}

TEST(lookup_ignores_case) {
    if (fat_ready() != 0) return 0;
    struct stat a, b;
    ASSERT_EQ(vfs_stat("/fat/hello.txt", &a), 0);
    ASSERT_EQ(vfs_stat("/fat/HELLO.TXT", &b), 0);
    ASSERT_EQ(a.st_ino, b.st_ino);
    ASSERT_EQ(vfs_stat("/fat/Subdir/Nested.txt", &b), 0);
    ASSERT_EQ(vfs_stat("/fat/missing.txt", &b), -ENOENT);
    ASSERT_EQ(vfs_stat("/fat/averyverylongname.txt", &b), -ENOENT);
    ASSERT_EQ(vfs_stat("/fat/hello.txt/x", &b), -ENOTDIR);
    return 0;
}

TEST(attributes) {
    if (fat_ready() != 0) return 0;
    struct stat st;
    ASSERT_EQ(vfs_stat("/fat/hello.txt", &st), 0);
    ASSERT_EQ(st.st_mode, S_IFREG | 0755);
    ASSERT_EQ(st.st_size, 13);
    ASSERT_EQ(st.st_blocks, 1);
    ASSERT_EQ(vfs_stat("/fat/ro.txt", &st), 0);
    ASSERT_EQ(st.st_mode, S_IFREG | 0555);   /* FAT read-only attribute */
    ASSERT_EQ(vfs_stat("/fat/subdir", &st), 0);
    ASSERT_EQ(st.st_mode, S_IFDIR | 0755);
    struct stat root;
    ASSERT_EQ(vfs_stat("/", &root), 0);
    ASSERT_NEQ(st.st_dev, root.st_dev);
    return 0;
}

TEST(reads) {
    if (fat_ready() != 0) return 0;
    ASSERT_EQ(vfs_env_read_file("/fat/hello.txt", text, sizeof(text)), 13);
    ASSERT_STR_EQ(text, "Hello, FAT32!");
    ASSERT_EQ(vfs_env_read_file("/fat/subdir/nested.txt", text, sizeof(text)), 10);
    ASSERT_STR_EQ(text, "1234567890");
    File *f;
    ASSERT_EQ(vfs_open("/fat/hello.txt", O_RDONLY, 0, &f), 0);
    char buf[16];
    IoBuf b = iob_kernel(buf, 5);
    ASSERT_EQ(vfs_pread(f, &b, 7), 5);
    ASSERT_MEM_EQ(buf, "FAT32", 5);
    ASSERT_EQ(vfs_pread(f, &b, 13), 0);   /* at the end */
    ASSERT_EQ(vfs_pread(f, &b, 1ll << 40), 0);
    b = iob_user(VFS_ENV_BAD_UADDR, 4);
    ASSERT_EQ(vfs_read(f, &b), -EFAULT);
    fput(f);
    return 0;
}

TEST(reads_across_clusters) {
    if (fat_ready() != 0) return 0;
    static char expect[16384];
    size_t len = 0;
    for (int i = 1; i <= 3000; i++) len += (size_t)snprintf(expect + len, sizeof(expect) - len, "%d\n", i);
    ASSERT_EQ(vfs_env_read_file("/fat/big.txt", text, sizeof(text)), (int64_t)len);
    ASSERT_MEM_EQ(text, expect, len);
    /* Small sequential reads, then seeks backwards and forwards */
    File *f;
    ASSERT_EQ(vfs_open("/fat/big.txt", O_RDONLY, 0, &f), 0);
    size_t got = 0;
    for (;;) {
        IoBuf b = iob_kernel(text + got, 100);
        int64_t n = vfs_read(f, &b);
        ASSERT_TRUE(n >= 0);
        if (n == 0) break;
        got += (size_t)n;
    }
    ASSERT_EQ(got, len);
    ASSERT_MEM_EQ(text, expect, len);
    loff_t offs[] = {9000, 10, 5000, 13000, 511, 512, 0};
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        char buf[700];
        IoBuf b = iob_kernel(buf, sizeof(buf));
        int64_t n = vfs_pread(f, &b, offs[i]);
        size_t want = len - (size_t)offs[i] < sizeof(buf) ? len - (size_t)offs[i] : sizeof(buf);
        ASSERT_EQ(n, (int64_t)want);
        ASSERT_MEM_EQ(buf, expect + offs[i], want);
    }
    fput(f);
    return 0;
}

TEST(changes_get_erofs) {
    if (fat_ready() != 0) return 0;
    File *f;
    ASSERT_EQ(vfs_open("/fat/hello.txt", O_WRONLY, 0, &f), -EROFS);
    ASSERT_EQ(vfs_open("/fat/hello.txt", O_RDWR, 0, &f), -EROFS);
    ASSERT_EQ(vfs_open("/fat/hello.txt", O_RDONLY | O_TRUNC, 0, &f), -EROFS);
    ASSERT_EQ(vfs_open("/fat/new.txt", O_CREAT | O_WRONLY, 0644, &f), -EROFS);
    ASSERT_EQ(vfs_mkdir("/fat/newdir", 0755), -EROFS);
    ASSERT_EQ(vfs_unlink("/fat/hello.txt"), -EROFS);
    ASSERT_EQ(vfs_rmdir("/fat/subdir"), -EROFS);
    ASSERT_EQ(vfs_rename("/fat/hello.txt", "/fat/bye.txt"), -EROFS);
    PathLoc loc;
    ASSERT_EQ(vfs_resolve("/fat/hello.txt", &loc), 0);
    InodeAttr a = {.valid = ATTR_MODE, .mode = 0600};
    ASSERT_EQ(vfs_setattr(loc.inode, &a), -EROFS);
    loc_put(&loc);
    ASSERT_EQ(vfs_open("/fat", O_RDONLY, 0, &f), 0);   /* reading the directory is fine */
    fput(f);
    return 0;
}

TEST(inodes_are_shared_and_leak_free) {
    if (fat_ready() != 0) return 0;
    File *a, *b;
    ASSERT_EQ(vfs_open("/fat/big.txt", O_RDONLY, 0, &a), 0);
    ASSERT_EQ(vfs_open("/fat/BIG.TXT", O_RDONLY, 0, &b), 0);
    ASSERT_TRUE(a->inode == b->inode);   /* one inode per entry (inode cache) */
    fput(a);
    fput(b);
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(vfs_env_read_file("/fat/subdir/nested.txt", text, sizeof(text)) > 0);
        Names l;
        ASSERT_EQ(list("/fat/subdir", &l), 0);
    }
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    return 0;
}

/* M1-12: file data lives in the page cache; drop_caches frees it and the
 * next read fills it again from the disk (pages span several clusters) */
TEST(reads_through_the_page_cache) {
    if (fat_ready() != 0) return 0;
    static char expect[16384];
    size_t len = 0;
    for (int i = 1; i <= 3000; i++) len += (size_t)snprintf(expect + len, sizeof(expect) - len, "%d\n", i);
    File *f;
    ASSERT_EQ(vfs_open("/fat/big.txt", O_RDONLY, 0, &f), 0);
    (void)filemap_drop_clean();
    FilemapStats s0, s;
    filemap_stats(&s0);
    IoBuf b = iob_kernel(text, sizeof(text));
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)len);
    ASSERT_MEM_EQ(text, expect, len);
    filemap_stats(&s);
    uint64_t pages = (len + 4095) / 4096;
    ASSERT_EQ(s.pgpgin - s0.pgpgin, pages);   /* one readahead window covers the file */
    ASSERT_EQ(s.file_pages - s0.file_pages, pages);
    ASSERT_EQ(filemap_of(f->inode)->nrpages, pages);
    b = iob_kernel(text, sizeof(text));
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)len);   /* cached: no disk read */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, pages);
    ASSERT_EQ(filemap_drop_clean(), pages);
    ASSERT_EQ(filemap_of(f->inode)->nrpages, 0);
    memset(text, 0, sizeof(text));
    b = iob_kernel(text, sizeof(text));
    ASSERT_EQ(vfs_pread(f, &b, 0), (int64_t)len);
    ASSERT_MEM_EQ(text, expect, len);
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 2 * pages);
    /* The bytes past the end in the last page are zeros */
    unsigned off;
    struct page *last = filemap_find(filemap_of(f->inode), pages - 1, &off);
    ASSERT_TRUE(last != NULL);
    ASSERT_EQ(((char *)page_to_virt(last))[len % 4096], 0);
    page_put(last);
    fput(f);
    return 0;
}

TestCase fat32_tests[] = {
    TEST_ENTRY(rejects_bad_boot_sectors),
    TEST_ENTRY(mounted_read_only),
    TEST_ENTRY(root_listing),
    TEST_ENTRY(lookup_ignores_case),
    TEST_ENTRY(attributes),
    TEST_ENTRY(reads),
    TEST_ENTRY(reads_across_clusters),
    TEST_ENTRY(changes_get_erofs),
    TEST_ENTRY(inodes_are_shared_and_leak_free),
    TEST_ENTRY(reads_through_the_page_cache),
};
int fat32_test_count = sizeof(fat32_tests) / sizeof(fat32_tests[0]);
