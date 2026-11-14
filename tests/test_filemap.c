/* arc_os — the page cache (M1-12) on the real radix.c, filemap.c and
 * filemap_mmap.c, over the VFS host environment: the radix tree's shape and
 * cost, readahead windows, dropping clean pages, ramfs counters and
 * write-back, and file-mapping faults (shared writes, private copies,
 * truncate taking entries away, SIGBUS past the end). */

#include "test_framework.h"
#include "vfs_env.h"

#include "drivers/blkdev.h"
#include "fs/vfs_internal.h"
#include "mm/filemap.h"
#include "mm/mm.h"
#include "proc/lockdep.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

/* --- Radix tree --- */

static int item[8];

TEST(radix_insert_lookup_delete) {
    vfs_env_setup();
    uint64_t base = radix_nodes_live();
    RadixTree t;
    radix_init(&t);
    ASSERT_TRUE(radix_lookup(&t, 0) == NULL);
    const uint64_t idx[] = {0, 63, 64, 4095, (1ull << 24) - 1, FILEMAP_MAX_INDEX};
    for (int i = 0; i < 6; i++) ASSERT_EQ(radix_insert(&t, idx[i], &item[i]), 0);
    ASSERT_EQ(radix_insert(&t, 64, &item[7]), -EEXIST);
    for (int i = 0; i < 6; i++) ASSERT_TRUE(radix_lookup(&t, idx[i]) == &item[i]);
    ASSERT_TRUE(radix_lookup(&t, 65) == NULL);
    ASSERT_TRUE(radix_lookup(&t, 1ull << 40) == NULL);   /* above the tree's reach */
    /* Ordered walk skips the holes */
    uint64_t at = 0, n = 0;
    for (void *p = radix_next(&t, 0, &at); p != NULL; p = radix_next(&t, at + 1, &at)) {
        ASSERT_EQ(at, idx[n]);
        n++;
        if (at == UINT64_MAX) break;
    }
    ASSERT_EQ(n, 6);
    for (int i = 5; i >= 0; i--) ASSERT_TRUE(radix_delete(&t, idx[i]) == &item[i]);
    ASSERT_TRUE(radix_delete(&t, 64) == NULL);
    ASSERT_EQ(t.height, 0);
    ASSERT_EQ(radix_nodes_live(), base);   /* every node freed as it emptied */
    return 0;
}

/* The cost line in radix.h: four levels below 64 GiB of pages */
TEST(radix_lookup_cost) {
    vfs_env_setup();
    RadixTree t;
    radix_init(&t);
    ASSERT_EQ(radix_insert(&t, 5, &item[0]), 0);
    (void)radix_lookup(&t, 5);
    ASSERT_EQ(radix_last_steps(), 1);
    ASSERT_EQ(radix_insert(&t, (1ull << 24) - 1, &item[1]), 0);   /* the last page below 64 GiB */
    (void)radix_lookup(&t, 5);
    ASSERT_EQ(radix_last_steps(), 4);
    ASSERT_TRUE(radix_delete(&t, (1ull << 24) - 1) == &item[1]);
    (void)radix_lookup(&t, 5);
    ASSERT_EQ(radix_last_steps(), 1);   /* the tree shrank back */
    ASSERT_TRUE(radix_delete(&t, 5) == &item[0]);
    return 0;
}

TEST(radix_out_of_memory) {
    vfs_env_setup();
    uint64_t base = radix_nodes_live();
    RadixTree t;
    radix_init(&t);
    ASSERT_EQ(radix_insert(&t, 1, &item[0]), 0);
    vfs_env_fail_alloc_in(2);   /* the second node of the new path */
    ASSERT_EQ(radix_insert(&t, 1ull << 20, &item[1]), -ENOMEM);
    vfs_env_fail_alloc_in(0);
    ASSERT_TRUE(radix_lookup(&t, 1) == &item[0]);
    ASSERT_TRUE(radix_lookup(&t, 1ull << 20) == NULL);
    ASSERT_EQ(radix_insert(&t, 1ull << 20, &item[1]), 0);
    ASSERT_TRUE(radix_delete(&t, 1ull << 20) == &item[1]);
    ASSERT_TRUE(radix_delete(&t, 1) == &item[0]);
    ASSERT_EQ(radix_nodes_live(), base);
    return 0;
}

/* --- A block-device mapping over memory: readahead and dropping --- */

#define DEV_PAGES 512
static uint8_t *dev_data;
static int dev_reads, dev_fail;

static int dev_read(BlockDevice *dev, uint64_t sector, uint32_t count, void *buf) {
    (void)dev;
    dev_reads++;
    if (dev_fail || (sector + count) * 512 > (uint64_t)DEV_PAGES * PAGE_SIZE) return -1;
    memcpy(buf, dev_data + sector * 512, (size_t)count * 512);
    return 0;
}

static BlockDevice mem_dev = {.read = dev_read};
static AddressSpace dev_as;

static void dev_setup(void) {
    vfs_env_setup();
    if (dev_data == NULL) {
        dev_data = malloc((size_t)DEV_PAGES * PAGE_SIZE);
        for (size_t i = 0; i < (size_t)DEV_PAGES * PAGE_SIZE; i++) dev_data[i] = (uint8_t)(i / PAGE_SIZE + i);
    }
    dev_fail = 0;
    (void)filemap_drop_clean();   /* other suites' clean pages: the counts below are this test's */
    filemap_bdev_init(&dev_as, &mem_dev, (loff_t)DEV_PAGES * PAGE_SIZE);
}

/* Get page index (reading it in as a reader with ra would), check its bytes, put it */
static int get_check(uint64_t index, FileRa *ra, int seq) {
    unsigned off;
    int err = 0;
    struct page *pg = filemap_get_page(&dev_as, index, ra, seq, &off, &err);
    if (pg == NULL) return err;
    int ok = off == 0 && memcmp(page_to_virt(pg), dev_data + index * PAGE_SIZE, PAGE_SIZE) == 0;
    page_put(pg);
    return ok ? 0 : -1;
}

TEST(readahead_windows) {
    dev_setup();
    FilemapStats s0, s;
    filemap_stats(&s0);
    FileRa ra = {0, 0};
    ASSERT_EQ(get_check(0, &ra, 0), 0);   /* first miss: 32 pages (128 KiB) */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 32);
    ASSERT_EQ(s.ra_pages - s0.ra_pages, 31);
    ASSERT_EQ(s.ra_batches - s0.ra_batches, 1);
    ASSERT_EQ(dev_as.nrpages, 32);
    ASSERT_EQ(get_check(31, &ra, 0), 0);   /* cached: no read */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 32);
    ASSERT_EQ(get_check(32, &ra, 0), 0);   /* sequential miss: the window doubles */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 32 + 64);
    ASSERT_EQ(get_check(96, &ra, 0), 0);   /* and again */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 32 + 64 + 128);
    ASSERT_EQ(get_check(300, &ra, 0), 0);   /* a jump: back to the smallest window */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 32 + 64 + 128 + 32);
    ASSERT_EQ(get_check(400, &ra, -1), 0);   /* MADV_RANDOM: just the page */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 32 + 64 + 128 + 32 + 1);
    ASSERT_EQ(get_check(450, &ra, 1), 0);   /* MADV_SEQUENTIAL: the largest window, cut at the end */
    filemap_stats(&s);
    ASSERT_EQ(s.pgpgin - s0.pgpgin, 32 + 64 + 128 + 32 + 1 + (DEV_PAGES - 450));
    ASSERT_EQ(get_check(DEV_PAGES, &ra, 0), -ERANGE);   /* past the end */
    /* WILLNEED: read the rest now, pages already there skipped */
    uint64_t before = dev_as.nrpages;
    ASSERT_EQ(filemap_readahead(&dev_as, 0, DEV_PAGES), DEV_PAGES - before);
    ASSERT_EQ(dev_as.nrpages, DEV_PAGES);
    for (uint64_t i = 0; i < DEV_PAGES; i += 37) ASSERT_EQ(get_check(i, NULL, 0), 0);
    filemap_stats(&s);
    ASSERT_EQ(s.file_pages - s0.file_pages, DEV_PAGES);
    /* Clean and unused: drop_caches frees every one */
    ASSERT_EQ(filemap_drop_clean(), DEV_PAGES);
    filemap_stats(&s);
    ASSERT_EQ(s.file_pages, s0.file_pages);
    ASSERT_EQ(s.dropped - s0.dropped, DEV_PAGES);
    ASSERT_EQ(dev_as.nrpages, 0);
    filemap_bdev_release(&dev_as);
    return 0;
}

TEST(drop_keeps_used_and_dirty_pages) {
    dev_setup();
    VfsEnvUsage u0, u1;
    vfs_env_usage(&u0);
    ASSERT_EQ(filemap_readahead(&dev_as, 0, 4), 4);
    unsigned off;
    int err;
    struct page *held = filemap_get_page(&dev_as, 1, NULL, 0, &off, &err);
    struct page *dirty = filemap_get_page(&dev_as, 2, NULL, 0, &off, &err);
    ASSERT_TRUE(held != NULL && dirty != NULL);
    filemap_set_dirty(dirty);
    page_put(dirty);
    ASSERT_EQ(filemap_drop_clean(), 2);   /* 0 and 3 */
    ASSERT_EQ(dev_as.nrpages, 2);
    page_put(held);
    ASSERT_EQ(filemap_drop_clean(), 1);   /* 1 now; 2 stays dirty */
    filemap_bdev_release(&dev_as);        /* drops the dirty one too (no write-back here) */
    ASSERT_EQ(dev_as.nrpages, 0);
    vfs_env_usage(&u1);
    ASSERT_TRUE(vfs_env_no_leak(&u0, &u1));
    return 0;
}

TEST(read_error_is_eio_and_retried) {
    dev_setup();
    dev_fail = 1;
    dev_reads = 0;
    ASSERT_EQ(get_check(7, NULL, 0), -EIO);
    ASSERT_EQ(dev_reads, 2);   /* one retry */
    dev_fail = 0;
    ASSERT_EQ(get_check(7, NULL, 0), 0);   /* the failed page reads again */
    filemap_bdev_release(&dev_as);
    return 0;
}

/* --- ramfs: the pages are the file --- */

static File *open_rw(const char *path) {
    File *f = NULL;
    return vfs_open(path, O_RDWR | O_CREAT, 0644, &f) == 0 ? f : NULL;
}

static int64_t pwr(File *f, const void *data, size_t n, loff_t at) {
    IoBuf b = iob_kernel((void *)data, n);
    return vfs_pwrite(f, &b, at);
}

static int64_t prd(File *f, void *data, size_t n, loff_t at) {
    IoBuf b = iob_kernel(data, n);
    return vfs_pread(f, &b, at);
}

TEST(ramfs_counters_and_fsync) {
    vfs_env_setup();
    VfsEnvUsage u0, u1;
    vfs_env_usage(&u0);
    FilemapStats s0, s;
    filemap_stats(&s0);
    File *f = open_rw("/tmp/pc_count");
    ASSERT_TRUE(f != NULL);
    static char buf[3 * 4096];
    memset(buf, 'x', sizeof(buf));
    ASSERT_EQ(pwr(f, buf, sizeof(buf), 0), (int64_t)sizeof(buf));
    filemap_stats(&s);
    ASSERT_EQ(s.file_pages - s0.file_pages, 3);
    ASSERT_EQ(s.dirty - s0.dirty, 3);
    ASSERT_EQ(f->inode->blocks, 24);
    ASSERT_EQ(vfs_fsync(f), 0);   /* nowhere to write: clean now */
    filemap_stats(&s);
    ASSERT_EQ(s.dirty, s0.dirty);
    ASSERT_EQ(filemap_drop_clean(), 0);   /* ramfs pages are never dropped */
    /* A hole costs nothing and reads as zeros */
    ASSERT_EQ(pwr(f, "y", 1, 10 * 4096), 1);
    filemap_stats(&s);
    ASSERT_EQ(s.file_pages - s0.file_pages, 4);
    char back[8];
    ASSERT_EQ(prd(f, back, 4, 5 * 4096), 4);
    ASSERT_EQ(back[0], 0);
    ASSERT_EQ(vfs_ftruncate(f, 1), 0);
    filemap_stats(&s);
    ASSERT_EQ(s.file_pages - s0.file_pages, 1);
    ASSERT_EQ(f->inode->blocks, 8);
    ASSERT_EQ(vfs_ftruncate(f, 4096), 0);   /* growing: the old tail reads as zeros */
    ASSERT_EQ(prd(f, back, 2, 0), 2);
    ASSERT_EQ(back[0], 'x');
    ASSERT_EQ(back[1], 0);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/pc_count"), 0);
    filemap_stats(&s);
    ASSERT_EQ(s.file_pages, s0.file_pages);
    vfs_env_usage(&u1);
    ASSERT_TRUE(vfs_env_no_leak(&u0, &u1));
    return 0;
}

TEST(write_past_16_tib_is_efbig) {
    vfs_env_setup();
    File *f = open_rw("/tmp/pc_big");
    ASSERT_TRUE(f != NULL);
    loff_t limit = (loff_t)(FILEMAP_MAX_INDEX + 1) << PAGE_SHIFT;
    ASSERT_EQ(pwr(f, "ab", 2, limit), -EFBIG);
    ASSERT_EQ(pwr(f, "ab", 2, limit - 1), 1);   /* up to the limit */
    ASSERT_EQ(f->inode->size, limit);
    ASSERT_EQ(vfs_ftruncate(f, 0), 0);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/pc_big"), 0);
    return 0;
}

/* --- File mappings --- */

#define MAP_AT 0x10000000ull

typedef struct {
    Mm mm;
    Vma v;
} Map;

static uint64_t next_pgd = 0x7000;

static void map_file(Map *m, File *f, uint64_t pages, uint32_t prot, uint32_t flags, uint64_t pgoff) {
    memset(m, 0, sizeof(*m));
    m->mm.pgd = next_pgd += 0x1000;
    spin_lock_init(&m->mm.pt_lock, LOCK_LEVEL_PAGE_TABLE, "pt");
    m->v.start = MAP_AT;
    m->v.end = MAP_AT + pages * PAGE_SIZE;
    m->v.prot = prot;
    m->v.flags = flags;
    m->v.file = f;
    m->v.pgoff = pgoff;
    m->v.ops = &filemap_vma_ops;
    m->v.mm = &m->mm;
    filemap_vma_ops.open(&m->v);
}

static void unmap_file(Map *m) {
    mm_zap_range(&m->mm, m->v.start, m->v.end, MM_ZAP_FLUSH);
    filemap_vma_ops.close(&m->v);
}

static int fault(Map *m, uint64_t page, int write) {
    return filemap_vma_ops.fault(&m->mm, &m->v, MAP_AT + page * PAGE_SIZE, write ? MM_ACCESS_WRITE : 0);
}

static struct page *mapped(Map *m, uint64_t page, uint32_t *flags) {
    struct page *pg = NULL;
    return vfs_env_pte(m->mm.pgd, MAP_AT + page * PAGE_SIZE, &pg, flags) ? pg : NULL;
}

static struct page *cached(File *f, uint64_t index) {
    unsigned off;
    struct page *pg = filemap_find(filemap_of(f->inode), index, &off);
    if (pg != NULL) page_put(pg);   /* the tree still holds it */
    return pg;
}

TEST(shared_mapping_is_the_cache) {
    vfs_env_setup();
    VfsEnvUsage u0, u1;
    vfs_env_usage(&u0);
    FilemapStats s0, s;
    filemap_stats(&s0);
    File *f = open_rw("/tmp/pc_shared");
    static char buf[2 * 4096];
    memset(buf, 'a', 4096);
    memset(buf + 4096, 'b', 4096);
    ASSERT_EQ(pwr(f, buf, sizeof(buf), 0), (int64_t)sizeof(buf));
    ASSERT_EQ(vfs_fsync(f), 0);
    Map m;
    map_file(&m, f, 2, VMA_READ | VMA_WRITE, VMA_SHARED | VMA_MAYWRITE, 0);
    /* A read maps the cache page itself, read-only (the first store must fault) */
    ASSERT_EQ(fault(&m, 0, 0), MM_FAULT_OK);
    uint32_t fl;
    struct page *pg = mapped(&m, 0, &fl);
    ASSERT_TRUE(pg != NULL && pg == cached(f, 0));
    ASSERT_EQ(fl, VMA_READ);
    ASSERT_EQ(pg->mapcount, 1);
    filemap_stats(&s);
    ASSERT_EQ(s.mapped - s0.mapped, 1);
    ASSERT_EQ(m.mm.rss, 1);
    /* The store: dirty, writable, and read() sees it */
    ASSERT_EQ(fault(&m, 0, 1), MM_FAULT_OK);
    ASSERT_TRUE(mapped(&m, 0, &fl) == pg);
    ASSERT_EQ(fl, VMA_READ | VMA_WRITE);
    ASSERT_TRUE(page_test(pg, PG_DIRTY));
    ((char *)page_to_virt(pg))[0] = 'Z';
    char c;
    ASSERT_EQ(prd(f, &c, 1, 0), 1);
    ASSERT_EQ(c, 'Z');
    /* write() is seen through the mapping */
    ASSERT_EQ(pwr(f, "Q", 1, 1), 1);
    ASSERT_EQ(((char *)page_to_virt(pg))[1], 'Q');
    /* Write-back write-protects the shared entry again */
    int flushes = vfs_env_tlb_flushes;
    ASSERT_EQ(filemap_vma_ops.sync(&m.v, m.v.start, m.v.end), 0);
    ASSERT_TRUE(mapped(&m, 0, &fl) == pg);
    ASSERT_EQ(fl, VMA_READ);
    ASSERT_TRUE(!page_test(pg, PG_DIRTY));
    ASSERT_TRUE(vfs_env_tlb_flushes > flushes);
    /* A write fault on a page not mapped yet: mapped writable, dirty */
    ASSERT_EQ(fault(&m, 1, 1), MM_FAULT_OK);
    ASSERT_TRUE(mapped(&m, 1, &fl) == cached(f, 1));
    ASSERT_EQ(fl, VMA_READ | VMA_WRITE);
    ASSERT_TRUE(page_test(cached(f, 1), PG_DIRTY));
    /* Truncate takes the entries away; a fault past the new end is SIGBUS */
    ASSERT_EQ(vfs_ftruncate(f, 100), 0);
    ASSERT_TRUE(mapped(&m, 1, NULL) == NULL);
    ASSERT_TRUE(mapped(&m, 0, NULL) == pg);   /* page 0 still holds bytes 0-99 */
    ASSERT_EQ(fault(&m, 1, 0), MM_FAULT_SIGBUS);
    ASSERT_EQ(vfs_ftruncate(f, 0), 0);
    ASSERT_TRUE(mapped(&m, 0, NULL) == NULL);
    ASSERT_EQ(fault(&m, 0, 0), MM_FAULT_SIGBUS);
    ASSERT_EQ(m.mm.rss, 0);
    filemap_stats(&s);
    ASSERT_EQ(s.mapped, s0.mapped);
    unmap_file(&m);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/pc_shared"), 0);
    vfs_env_usage(&u1);
    ASSERT_TRUE(vfs_env_no_leak(&u0, &u1));
    return 0;
}

TEST(private_mapping_copies_on_write) {
    vfs_env_setup();
    VfsEnvUsage u0, u1;
    vfs_env_usage(&u0);
    File *f = open_rw("/tmp/pc_private");
    static char buf[3 * 4096];
    memset(buf, 'p', sizeof(buf));
    ASSERT_EQ(pwr(f, buf, sizeof(buf), 0), (int64_t)sizeof(buf));
    ASSERT_EQ(vfs_fsync(f), 0);   /* clean: a private store must not dirty it */
    Map m;
    map_file(&m, f, 3, VMA_READ | VMA_WRITE, VMA_MAYWRITE, 0);
    /* Read: the cache page, read-only */
    ASSERT_EQ(fault(&m, 0, 0), MM_FAULT_OK);
    uint32_t fl;
    struct page *pg = mapped(&m, 0, &fl);
    ASSERT_TRUE(pg == cached(f, 0));
    ASSERT_EQ(fl, VMA_READ);
    /* Write: a private copy replaces it; the file never sees the store */
    ASSERT_EQ(fault(&m, 0, 1), MM_FAULT_OK);
    struct page *cp = mapped(&m, 0, &fl);
    ASSERT_TRUE(cp != NULL && cp != pg);
    ASSERT_TRUE(!(cp->flags & PG_FILE));
    ASSERT_EQ(fl, VMA_READ | VMA_WRITE);
    ASSERT_EQ(pg->mapcount, 0);
    ASSERT_TRUE(!page_test(pg, PG_DIRTY));
    ((char *)page_to_virt(cp))[0] = 'W';
    char c;
    ASSERT_EQ(prd(f, &c, 1, 0), 1);
    ASSERT_EQ(c, 'p');
    /* A write to a page never mapped copies straight away */
    ASSERT_EQ(fault(&m, 2, 1), MM_FAULT_OK);
    cp = mapped(&m, 2, &fl);
    ASSERT_TRUE(cp != NULL && !(cp->flags & PG_FILE));
    ASSERT_EQ(((char *)page_to_virt(cp))[4095], 'p');
    /* A later write() to the file is not seen by the private copy */
    ASSERT_EQ(pwr(f, "N", 1, 2 * 4096), 1);
    ASSERT_EQ(((char *)page_to_virt(cp))[0], 'p');
    /* ...but is by a page still shared with the cache */
    ASSERT_EQ(fault(&m, 1, 0), MM_FAULT_OK);
    ASSERT_EQ(pwr(f, "M", 1, 4096), 1);
    ASSERT_EQ(((char *)page_to_virt(mapped(&m, 1, NULL)))[0], 'M');
    /* Truncate takes the private copies too (Linux does) */
    ASSERT_EQ(vfs_ftruncate(f, 0), 0);
    for (int i = 0; i < 3; i++) ASSERT_TRUE(mapped(&m, (uint64_t)i, NULL) == NULL);
    ASSERT_EQ(m.mm.rss, 0);
    unmap_file(&m);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/pc_private"), 0);
    vfs_env_usage(&u1);
    ASSERT_TRUE(vfs_env_no_leak(&u0, &u1));
    return 0;
}

/* Two address spaces share one cache page; munmap of one leaves the other */
TEST(two_mappings_share_pages) {
    vfs_env_setup();
    File *f = open_rw("/tmp/pc_two");
    ASSERT_EQ(pwr(f, "shared", 6, 0), 6);
    Map a, b;
    map_file(&a, f, 1, VMA_READ, VMA_SHARED, 0);
    map_file(&b, f, 1, VMA_READ, 0, 0);
    ASSERT_EQ(fault(&a, 0, 0), MM_FAULT_OK);
    ASSERT_EQ(fault(&b, 0, 0), MM_FAULT_OK);
    struct page *pg = cached(f, 0);
    ASSERT_TRUE(mapped(&a, 0, NULL) == pg && mapped(&b, 0, NULL) == pg);
    ASSERT_EQ(pg->mapcount, 2);
    ASSERT_EQ(pg->refcount, 3);   /* the tree and two entries */
    unmap_file(&a);
    ASSERT_EQ(pg->mapcount, 1);
    ASSERT_TRUE(mapped(&b, 0, NULL) == pg);
    unmap_file(&b);
    ASSERT_EQ(pg->refcount, 1);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/pc_two"), 0);
    return 0;
}

/* A mapping with an offset maps the file's pages from there; past the end: SIGBUS */
TEST(mapping_offset_and_eof) {
    vfs_env_setup();
    File *f = open_rw("/tmp/pc_off");
    static char buf[3 * 4096];
    for (int i = 0; i < 3; i++) memset(buf + i * 4096, '0' + i, 4096);
    ASSERT_EQ(pwr(f, buf, sizeof(buf) - 100, 0), (int64_t)sizeof(buf) - 100);
    Map m;
    map_file(&m, f, 4, VMA_READ, 0, 1);   /* file pages 1..4; the file has 3 */
    ASSERT_EQ(fault(&m, 0, 0), MM_FAULT_OK);
    ASSERT_EQ(((char *)page_to_virt(mapped(&m, 0, NULL)))[0], '1');
    ASSERT_EQ(fault(&m, 1, 0), MM_FAULT_OK);   /* the last, partial page */
    char *last = page_to_virt(mapped(&m, 1, NULL));
    ASSERT_EQ(last[4095 - 100], '2');
    ASSERT_EQ(last[4096 - 100], 0);   /* past the end inside the page: zeros */
    ASSERT_EQ(fault(&m, 2, 0), MM_FAULT_SIGBUS);
    unmap_file(&m);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/pc_off"), 0);
    return 0;
}

TestCase filemap_tests[] = {
    TEST_ENTRY(radix_insert_lookup_delete),
    TEST_ENTRY(radix_lookup_cost),
    TEST_ENTRY(radix_out_of_memory),
    TEST_ENTRY(readahead_windows),
    TEST_ENTRY(drop_keeps_used_and_dirty_pages),
    TEST_ENTRY(read_error_is_eio_and_retried),
    TEST_ENTRY(ramfs_counters_and_fsync),
    TEST_ENTRY(write_past_16_tib_is_efbig),
    TEST_ENTRY(shared_mapping_is_the_cache),
    TEST_ENTRY(private_mapping_copies_on_write),
    TEST_ENTRY(two_mappings_share_pages),
    TEST_ENTRY(mapping_offset_and_eof),
};
int filemap_test_count = sizeof(filemap_tests) / sizeof(filemap_tests[0]);
