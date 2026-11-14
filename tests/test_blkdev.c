/* arc_os — Host-side tests for kernel/drivers/blkdev.c (M1-10)
 *
 * The real block layer against a fake disk driver: requests wait in the fake
 * device until the test "raises the interrupt" (blk_complete), which copies
 * data between the device's store and the request's pages. Memory is a test
 * arena with a real page descriptor array, so pages, offsets and the kernel
 * address walk of blk_rw_sync are the kernel's. Not waiting means polling:
 * sched_can_block() is 0 here, as at boot. */

#include "test_framework.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guard what the fakes below replace */
#define ARCHOS_MM_SLAB_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_MM_VMM_H
#define ARCHOS_MM_PMM_H
#define ARCHOS_PROC_SEMAPHORE_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_ARCH_HAL_H
#define ARCHOS_DIAG_DIAG_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_STRING_H

#include "mm/gfp.h"
#include "arch/hal_lock.h"
#include "drivers/blkdev.h"

/* The real page.h over this test's own descriptor array */
#define pmm_page_array  blkt_page_array
#define pmm_max_pfn     blkt_max_pfn
#define pmm_hhdm_offset blkt_hhdm_offset
#include "mm/page.h"

/* Test pages, then a bump heap for kmalloc (kernel heap memory is HHDM) */
#define TEST_PAGES  64
#define HEAP_PAGES  256
#define ARENA_PAGES (TEST_PAGES + HEAP_PAGES)
static uint8_t arena[ARENA_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static size_t heap_used;
static struct page arena_pages[ARENA_PAGES + 1];
struct page *blkt_page_array = arena_pages;
uint64_t blkt_max_pfn = ARENA_PAGES + 1;
uint64_t blkt_hhdm_offset;   /* the arena starts at physical page 1 (0 means "not mapped") */

static uint64_t vmm_get_phys(uint64_t va) {
    uint64_t base = (uint64_t)(uintptr_t)arena;
    if (va < base || va >= base + sizeof(arena)) return 0;
    return va - blkt_hhdm_offset;
}

/* Allocators with counters */
static int rq_live, rq_allocs;
struct kmem_cache {
    int unused;
};
static int kmem_cache_setup(struct kmem_cache *c, const char *n, size_t size, size_t align) {
    (void)c, (void)n, (void)size, (void)align;
    return 0;
}
static int fail_rq_alloc;
static void *kmem_cache_alloc(struct kmem_cache *c, uint32_t flags) {
    (void)c, (void)flags;
    if (fail_rq_alloc && --fail_rq_alloc == 0) return NULL;
    rq_live++;
    rq_allocs++;
    return malloc(sizeof(BlkRequest));
}
static void kmem_cache_free(struct kmem_cache *c, void *p) {
    (void)c;
    rq_live--;
    free(p);
}
static void *kmalloc(size_t size, uint32_t flags) {
    (void)flags;
    size = (size + 15) & ~(size_t)15;
    if (heap_used + size > HEAP_PAGES * PAGE_SIZE) abort();
    void *p = arena + TEST_PAGES * PAGE_SIZE + heap_used;
    heap_used += size;
    memset(p, 0, size);
    return p;
}
static void kfree(void *p) {
    (void)p;   /* the bump heap is never reused */
}
/* Bounce pages: page-aligned from the bump heap, counted */
static int pages_live;
static struct page *alloc_pages(unsigned order, uint32_t flags) {
    (void)flags;
    heap_used = (heap_used + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    uint8_t *p = kmalloc((size_t)PAGE_SIZE << order, 0);
    pages_live++;
    return pfn_to_page((uint64_t)((uintptr_t)p - blkt_hhdm_offset) >> PAGE_SHIFT);
}
static void free_pages(struct page *pg, unsigned order) {
    (void)pg, (void)order;
    pages_live--;
}

/* One thread: a semaphore is a count, and waiting never blocks (polling) */
typedef struct {
    int32_t count;
} Semaphore;
static void sem_init(Semaphore *s, int32_t n) {
    s->count = n;
}
static void sem_post(Semaphore *s) {
    s->count++;
}
static void sem_wait(Semaphore *s) {
    if (s->count <= 0) abort();
    s->count--;
}
static int sem_trywait(Semaphore *s) {
    if (s->count <= 0) return -1;
    s->count--;
    return 0;
}
static int sched_can_block(void) {
    return 0;
}

/* Time: a fake TSC at 1000 cycles per ms */
static uint64_t fake_now;
static uint64_t hal_tsc_read(void) {
    return fake_now;
}
static uint64_t diag_ms_to_cycles(uint32_t ms) {
    return (uint64_t)ms * 1000;
}

static char last_log[256];
static void kprintf(const char *fmt, ...) {
    strncpy(last_log, fmt, sizeof(last_log) - 1);
}
int ksnprintf(char *buf, size_t size, const char *fmt, ...);   /* the real one (test_kprintf.c) */

#include "../kernel/drivers/blkdev.c"
#include "../kernel/drivers/blk_lookup.c"
#include "arc_disk.h"

/* --- The fake disk --- */

#define FAKE_SLOTS 256

typedef struct {
    BlockDevice disk;
    uint8_t *store;
    BlkRequest *slot[FAKE_SLOTS];
    unsigned nslots, cap;
    unsigned budget;      /* completions per poll (0 = all) */
    int stuck;            /* poll completes nothing (a hung device) */
    int kicks, resets, queued, fail_next;
    unsigned max_chain_segs;
} FakeDisk;

static void fake_io(FakeDisk *fd, BlkRequest *head) {
    uint64_t pos = head->sector * BLK_SECTOR_SIZE;
    BlkRequest *r;
    uint32_t i;
    blk_rq_for_each_seg(head, r, i) {
        uint8_t *mem = (uint8_t *)page_to_virt(r->seg[i].page) + r->seg[i].offset;
        if (head->op == BLK_OP_READ)
            memcpy(mem, fd->store + pos, r->seg[i].len);
        else
            memcpy(fd->store + pos, mem, r->seg[i].len);
        pos += r->seg[i].len;
    }
}

static int fake_queue_rq(BlockDevice *disk, BlkRequest *rq) {
    FakeDisk *fd = disk->driver_data;
    if (fd->nslots >= fd->cap) return BLK_BUSY;
    if (fd->fail_next) {
        fd->fail_next = 0;
        return -EIO;
    }
    if (rq->total_segs > fd->max_chain_segs) fd->max_chain_segs = rq->total_segs;
    fd->slot[fd->nslots++] = rq;
    fd->queued++;
    return 0;
}

static void fake_kick(BlockDevice *disk) {
    ((FakeDisk *)disk->driver_data)->kicks++;
}

static void fake_poll(BlockDevice *disk) {
    FakeDisk *fd = disk->driver_data;
    if (fd->stuck) return;
    unsigned n = fd->budget != 0 && fd->budget < fd->nslots ? fd->budget : fd->nslots;
    for (unsigned k = 0; k < n; k++) {
        BlkRequest *rq = fd->slot[k];
        if (rq->op != BLK_OP_FLUSH) fake_io(fd, rq);
        blk_rq_done(rq, 0);
    }
    memmove(fd->slot, fd->slot + n, (fd->nslots - n) * sizeof(fd->slot[0]));
    fd->nslots -= n;
}

static void fake_reset(BlockDevice *disk) {
    FakeDisk *fd = disk->driver_data;
    fd->resets++;
    fd->nslots = 0;
}

static const BlockOps fake_ops = {fake_queue_rq, fake_kick, fake_poll, fake_reset};

#define STORE_BYTES (8u << 20)

/* A fresh fake disk; store_from: an image file to load, or NULL (pattern) */
static FakeDisk *fake_disk(uint32_t lbs, uint32_t depth, uint32_t max_segs, const char *store_from) {
    blkt_hhdm_offset = (uint64_t)(uintptr_t)arena - PAGE_SIZE;
    FakeDisk *fd = calloc(1, sizeof(FakeDisk));
    fd->store = calloc(1, STORE_BYTES);
    if (store_from != NULL) {
        FILE *f = fopen(store_from, "rb");
        if (f == NULL || fread(fd->store, 1, STORE_BYTES, f) != STORE_BYTES) printf("    (missing %s)\n", store_from);
        if (f) fclose(f);
    } else {
        for (uint64_t s = 0; s < STORE_BYTES / 8; s++) ((uint64_t *)fd->store)[s] = s / 64;   /* word = its sector */
    }
    fd->cap = FAKE_SLOTS;
    fd->disk.nr_sectors = STORE_BYTES / BLK_SECTOR_SIZE;
    fd->disk.logical_block = fd->disk.physical_block = lbs;
    fd->disk.max_segments = max_segs;
    fd->disk.max_transfer = 1u << 20;
    fd->disk.flags = BLK_CACHE;
    fd->disk.ops = &fake_ops;
    fd->disk.driver_data = fd;
    if (blk_disk_register(&fd->disk, "td", depth) != 0) abort();
    /* Count from here on, not the GPT scan's reads */
    fd->kicks = fd->queued = 0;
    fd->max_chain_segs = 0;
    memset(&fd->disk.queue->stats, 0, sizeof(fd->disk.queue->stats));
    return fd;
}

static int ended, end_status;
static void count_end(void *priv, int status) {
    (void)priv;
    ended++;
    if (status != 0) end_status = status;
}

static struct page *apage(unsigned i) {
    return pfn_to_page(i + 1);
}

static BlkRequest *read_rq(BlockDevice *dev, uint64_t sector, unsigned page) {
    BlkRequest *rq = blk_rq_alloc(dev, BLK_OP_READ, sector, count_end, NULL);
    blk_rq_add_page(rq, apage(page), 0, PAGE_SIZE);
    return rq;
}

static int words_are_sector(const void *buf, uint64_t sector, size_t bytes) {
    const uint64_t *w = buf;
    for (size_t i = 0; i < bytes / 8; i++) {
        if (w[i] != sector + i / 64) return 0;
    }
    return 1;
}

/* --- Tests --- */

TEST(blk_register_scans_gpt) {
    FakeDisk *fd = fake_disk(512, 32, 16, "tests/gpt/gpt512.img");
    BlockDevice *sys = blk_find_label("arc-system", NULL);
    ASSERT_TRUE(fd->disk.has_gpt);
    ASSERT_TRUE(sys != NULL);
    ASSERT_TRUE(sys->disk == &fd->disk);
    ASSERT_EQ(sys->start, 2048);
    ASSERT_EQ(sys->nr_sectors, 2048);
    ASSERT_EQ(sys->partno, 1);
    char name[20];
    snprintf(name, sizeof(name), "%s2", fd->disk.name);
    BlockDevice *data = blk_find(name);
    ASSERT_TRUE(data != NULL);
    ASSERT_STR_EQ(data->label, "arc-data");
    Guid t;
    gpt_guid_parse(ARC_PART_TYPE_DATA, &t);
    ASSERT_TRUE(blk_find_part(&fd->disk, &t) == data);
    Guid other = fd->disk.disk_guid;
    other.b[0] ^= 1;
    ASSERT_TRUE(blk_find_part(data, &t) == NULL); /* a partition is no disk */
    ASSERT_TRUE(blk_find_label("no-such-label", NULL) == NULL);
    /* M1-13's lookups: the disk by GUID, a label that must also have the
     * type, a PARTUUID */
    ASSERT_TRUE(blk_find_disk(&fd->disk.disk_guid) == &fd->disk);
    ASSERT_TRUE(blk_find_disk(&other) == NULL);
    ASSERT_TRUE(blk_find_label("arc-data", &t) == data);
    ASSERT_TRUE(blk_find_label("arc-system", &t) == NULL);
    ASSERT_TRUE(blk_find_partuuid(&data->part_guid) == data);
    ASSERT_TRUE(blk_find_partuuid(&fd->disk.disk_guid) == NULL);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_partition_offset_and_bounds) {
    BlockDevice *data = blk_find_label("arc-data", NULL); /* on the previous test's disk */
    ASSERT_TRUE(data != NULL);
    FakeDisk *fd = data->disk->driver_data;
    memset(fd->store + data->start * 512, 0xAB, 512);
    uint8_t *buf = arena;
    ASSERT_EQ(blk_rw_sync(data, BLK_OP_READ, 0, buf, 512), 0);
    ASSERT_EQ(buf[0], 0xAB);
    ASSERT_EQ(buf[511], 0xAB);
    /* Last sector of the partition is fine, one past it is not */
    ASSERT_EQ(blk_rw_sync(data, BLK_OP_READ, data->nr_sectors - 1, buf, 512), 0);
    ASSERT_EQ(blk_rw_sync(data, BLK_OP_READ, data->nr_sectors - 1, buf, 1024), -EINVAL);
    ASSERT_EQ(blk_rw_sync(data, BLK_OP_READ, data->nr_sectors, buf, 512), -EINVAL);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_many_in_flight_one_kick) {
    FakeDisk *fd = fake_disk(512, 128, 16, NULL);
    BlkPlug plug;
    blk_plug_init(&plug);
    ended = end_status = 0;
    for (unsigned i = 0; i < 64; i++) blk_plug_add(&plug, read_rq(&fd->disk, (uint64_t)i * 64, i % TEST_PAGES));
    blk_plug_flush(&plug);
    ASSERT_EQ(fd->kicks, 1);
    ASSERT_EQ(fd->nslots, 64);
    ASSERT_EQ(fd->disk.queue->in_flight, 64);
    ASSERT_EQ(ended, 0);
    blk_complete(&fd->disk);   /* the interrupt */
    ASSERT_EQ(ended, 64);
    ASSERT_EQ(end_status, 0);
    ASSERT_TRUE(words_are_sector(page_to_virt(apage(63)), 63 * 64, PAGE_SIZE));
    BlkStats st;
    blk_get_stats(&fd->disk, &st);
    ASSERT_EQ(st.max_in_flight, 64);
    ASSERT_EQ(st.merged, 0);
    ASSERT_EQ(st.in_flight, 0);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_depth_limit_and_out_of_order_tags) {
    FakeDisk *fd = fake_disk(512, 4, 16, NULL);
    BlkPlug plug;
    blk_plug_init(&plug);
    ended = 0;
    for (unsigned i = 0; i < 10; i++) blk_plug_add(&plug, read_rq(&fd->disk, (uint64_t)i * 64, i));
    blk_plug_flush(&plug);
    ASSERT_EQ(fd->nslots, 4);   /* the rest wait in the queue */
    fd->budget = 2;
    blk_complete(&fd->disk);
    ASSERT_EQ(ended, 2);
    ASSERT_EQ(fd->nslots, 4);   /* two finished, two more started */
    ASSERT_EQ(fd->slot[3]->sector, 5 * 64);
    fd->budget = 0;
    for (int k = 0; k < 4 && ended < 10; k++) blk_complete(&fd->disk);
    ASSERT_EQ(ended, 10);
    ASSERT_EQ(fd->disk.queue->nfree_tags, 4);
    for (unsigned i = 0; i < 10; i++) ASSERT_TRUE(words_are_sector(page_to_virt(apage(i)), i * 64, PAGE_SIZE));
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_plug_merges_adjacent_within_limits) {
    FakeDisk *fd = fake_disk(512, 32, 3, NULL);   /* 3 segments per device request */
    BlkPlug plug;
    blk_plug_init(&plug);
    ended = 0;
    for (unsigned i = 0; i < 8; i++) blk_plug_add(&plug, read_rq(&fd->disk, 800 + (uint64_t)i * 8, i));
    blk_plug_flush(&plug);
    ASSERT_EQ(fd->nslots, 3);   /* 3 + 3 + 2 */
    ASSERT_EQ(fd->max_chain_segs, 3);
    blk_complete(&fd->disk);
    ASSERT_EQ(ended, 8);
    for (unsigned i = 0; i < 8; i++) ASSERT_TRUE(words_are_sector(page_to_virt(apage(i)), 800 + i * 8, PAGE_SIZE));
    BlkStats st;
    blk_get_stats(&fd->disk, &st);
    ASSERT_EQ(st.submitted, 8);
    ASSERT_EQ(st.merged, 5);
    ASSERT_EQ(st.dispatched, 3);
    /* Not adjacent, or another op: no merge */
    blk_plug_init(&plug);
    blk_plug_add(&plug, read_rq(&fd->disk, 0, 0));
    blk_plug_add(&plug, read_rq(&fd->disk, 16, 1));
    BlkRequest *w = blk_rq_alloc(&fd->disk, BLK_OP_WRITE, 24, count_end, NULL);
    blk_rq_add_page(w, apage(2), 0, PAGE_SIZE);
    blk_plug_add(&plug, w);
    blk_plug_flush(&plug);
    ASSERT_EQ(fd->nslots, 3);
    blk_complete(&fd->disk);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_merge_into_pending_tail) {
    FakeDisk *fd = fake_disk(512, 1, 16, NULL);   /* one in flight: the rest wait */
    ended = 0;
    blk_submit(read_rq(&fd->disk, 0, 0));
    blk_submit(read_rq(&fd->disk, 100, 1));
    blk_submit(read_rq(&fd->disk, 108, 2));   /* follows the pending one */
    ASSERT_EQ(fd->nslots, 1);
    ASSERT_TRUE(fd->disk.queue->pend_head == fd->disk.queue->pend_tail);
    blk_complete(&fd->disk);
    ASSERT_EQ(fd->nslots, 1);
    ASSERT_EQ(fd->slot[0]->total_segs, 2);
    blk_complete(&fd->disk);
    ASSERT_EQ(ended, 3);
    ASSERT_TRUE(words_are_sector(page_to_virt(apage(2)), 108, PAGE_SIZE));
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_device_busy_requeues_in_order) {
    FakeDisk *fd = fake_disk(512, 16, 16, NULL);
    fd->cap = 2;   /* the device has room for two */
    BlkPlug plug;
    blk_plug_init(&plug);
    ended = 0;
    for (unsigned i = 0; i < 5; i++) blk_plug_add(&plug, read_rq(&fd->disk, (uint64_t)i * 64, i));
    blk_plug_flush(&plug);
    ASSERT_EQ(fd->nslots, 2);
    ASSERT_EQ(fd->disk.queue->in_flight, 2);
    ASSERT_EQ(fd->disk.queue->nfree_tags, 14);
    ASSERT_EQ(fd->disk.queue->pend_head->sector, 2 * 64);
    for (int k = 0; k < 3; k++) blk_complete(&fd->disk);
    ASSERT_EQ(ended, 5);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_sync_splits_unaligned_buffer) {
    FakeDisk *fd = fake_disk(512, 8, 3, NULL);
    uint8_t *buf = arena + 512;   /* starts mid-page: 11 pages touched */
    memset(arena, 0, 12 * PAGE_SIZE);
    int before = fd->queued;
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 40, buf, 10 * PAGE_SIZE), 0);
    ASSERT_TRUE(words_are_sector(buf, 40, 10 * PAGE_SIZE));
    ASSERT_EQ(arena[511], 0);   /* nothing before the buffer */
    ASSERT_EQ(fd->queued - before, 4);   /* 11 segments, 3 per request */
    ASSERT_EQ(fd->max_chain_segs, 3);
    /* Write back through the same path */
    memset(buf, 0x77, 3 * PAGE_SIZE);
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_WRITE, 40, buf, 3 * PAGE_SIZE), 0);
    ASSERT_EQ(fd->store[40 * 512], 0x77);
    ASSERT_EQ(fd->store[40 * 512 + 3 * PAGE_SIZE - 1], 0x77);
    ASSERT_EQ(fd->store[40 * 512 + 3 * PAGE_SIZE], 64);   /* untouched: sector 64's word */
    ASSERT_EQ(rq_live, 0);
    return 0;
}

/* A device without SEG_MAX takes one segment per request: a block that
 * straddles two pages (fat32_mount's stack buffer can) goes through a bounce
 * page instead of failing with -EINVAL */
TEST(blk_one_segment_bounces_straddling_blocks) {
    FakeDisk *fd = fake_disk(512, 8, 1, NULL);
    uint8_t *buf = arena + PAGE_SIZE - 200; /* one 512-byte block across two pages */
    memset(arena, 0, 3 * PAGE_SIZE);
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 7, buf, 512), 0);
    ASSERT_TRUE(words_are_sector(buf, 7, 512));
    ASSERT_EQ(buf[-1], 0);
    ASSERT_EQ(buf[512], 0);
    /* Several pages from an unaligned start: aligned pieces go direct, the
     * blocks that straddle a page boundary bounce */
    buf = arena + 104;
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 40, buf, 3 * PAGE_SIZE), 0);
    ASSERT_TRUE(words_are_sector(buf, 40, 3 * PAGE_SIZE));
    ASSERT_EQ(fd->max_chain_segs, 1);
    /* Writes bounce too */
    buf = arena + PAGE_SIZE - 200;
    memset(buf, 0x33, 512);
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_WRITE, 300, buf, 512), 0);
    ASSERT_EQ(fd->store[300 * 512], 0x33);
    ASSERT_EQ(fd->store[300 * 512 + 511], 0x33);
    ASSERT_EQ(fd->store[301 * 512], 301 & 0xFF); /* next sector untouched */
    ASSERT_EQ(pages_live, 0);
    ASSERT_EQ(rq_live, 0);

    /* 4Kn, one segment: every block of an unaligned buffer straddles */
    FakeDisk *fk = fake_disk(4096, 8, 1, NULL);
    buf = arena + 1024;
    ASSERT_EQ(blk_rw_sync(&fk->disk, BLK_OP_READ, 64, buf, 2 * PAGE_SIZE), 0);
    ASSERT_TRUE(words_are_sector(buf, 64, 2 * PAGE_SIZE));
    ASSERT_EQ(pages_live, 0);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_4kn_requests_cut_on_block_boundaries) {
    FakeDisk *fd = fake_disk(4096, 8, 2, NULL);
    uint8_t *buf = arena + 1024;   /* two segments per request: 3 KiB + 4 KiB, cut to 4 KiB */
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 64, buf, 4 * PAGE_SIZE), 0);
    ASSERT_TRUE(words_are_sector(buf, 64, 4 * PAGE_SIZE));
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 1, buf, PAGE_SIZE), -EINVAL);   /* not on a block */
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 0, buf, 512), -EINVAL);         /* part of a block */
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_refusals_and_flush) {
    FakeDisk *fd = fake_disk(512, 8, 16, NULL);
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 0, arena + 2, 512), -EINVAL);   /* not 4-byte aligned */
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 0, (void *)8, 512), -EFAULT);   /* no page behind it */
    int q = fd->queued;
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_FLUSH, 0, NULL, 0), 0);
    ASSERT_EQ(fd->queued, q + 1);   /* write cache: the device sees it */
    fd->disk.flags = BLK_RDONLY;
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_FLUSH, 0, NULL, 0), 0);
    ASSERT_EQ(fd->queued, q + 1);   /* no cache: nothing to do */
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_WRITE, 0, arena, 512), -EROFS);
    fd->fail_next = 1;
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 0, arena, 512), -EIO);
    fail_rq_alloc = 1;
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 0, arena, 512), -ENOMEM);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

static int page_status = 1;
static void page_end(void *priv, int status) {
    *(int *)priv = status;
}

TEST(blk_submit_page_async) {
    FakeDisk *fd = fake_disk(512, 8, 16, NULL);
    page_status = 1;
    ASSERT_EQ(blk_submit_page(&fd->disk, BLK_OP_READ, 128, apage(5), 0, PAGE_SIZE, page_end, &page_status), 0);
    ASSERT_EQ(page_status, 1);   /* not before the interrupt */
    /* Boot polling (a page wait before the scheduler) reaps it as the
     * interrupt would; with nothing in flight it does nothing */
    blk_poll_all();
    ASSERT_EQ(page_status, 0);
    blk_poll_all();
    ASSERT_TRUE(words_are_sector(page_to_virt(apage(5)), 128, PAGE_SIZE));
    /* A refused request still ends, at once */
    ASSERT_EQ(blk_submit_page(&fd->disk, BLK_OP_READ, 1ull << 40, apage(5), 0, PAGE_SIZE, page_end, &page_status), 0);
    ASSERT_EQ(page_status, -EINVAL);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_lost_interrupt_completed_by_poll) {
    FakeDisk *fd = fake_disk(512, 8, 16, NULL);
    blk_test_set(&fd->disk, 100, 1);
    ended = 0;
    fake_now = 1000000;
    blk_submit(read_rq(&fd->disk, 0, 0));
    blk_complete(&fd->disk);   /* dropped */
    ASSERT_EQ(ended, 0);
    fake_now += 99 * 1000;
    blk_expire_all(fake_now);   /* not yet due */
    ASSERT_EQ(ended, 0);
    fake_now += 2 * 1000;
    blk_expire_all(fake_now);
    ASSERT_EQ(ended, 1);
    BlkStats st;
    blk_get_stats(&fd->disk, &st);
    ASSERT_EQ(st.polled, 1);
    ASSERT_EQ(st.timeouts, 0);
    ASSERT_EQ(fd->resets, 0);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_hung_device_reset_fails_everything) {
    FakeDisk *fd = fake_disk(512, 2, 16, NULL);
    blk_test_set(&fd->disk, 100, 0);
    fd->stuck = 1;
    ended = end_status = 0;
    fake_now = 5000000;
    for (unsigned i = 0; i < 5; i++) blk_submit(read_rq(&fd->disk, (uint64_t)i * 64, i));
    ASSERT_EQ(fd->disk.queue->in_flight, 2);
    fake_now += 101 * 1000;
    blk_expire_all(fake_now);
    ASSERT_EQ(fd->resets, 1);
    ASSERT_EQ(ended, 5);   /* flying and waiting alike */
    ASSERT_EQ(end_status, -EIO);
    ASSERT_EQ(fd->disk.queue->in_flight, 0);
    ASSERT_EQ(fd->disk.queue->nfree_tags, 2);
    /* The device is gone: later requests fail at once */
    ASSERT_EQ(blk_rw_sync(&fd->disk, BLK_OP_READ, 0, arena, 512), -EIO);
    ASSERT_EQ(fd->queued, 2);
    ASSERT_EQ(rq_live, 0);
    return 0;
}

TEST(blk_names) {
    FakeDisk *fd = fake_disk(512, 8, 16, NULL);
    ASSERT_EQ(strncmp(fd->disk.name, "td", 2), 0);
    ASSERT_TRUE(blk_find(fd->disk.name) == &fd->disk);
    char out[16];
    part_name(out, "vda", 2);
    ASSERT_STR_EQ(out, "vda2");
    part_name(out, "nvme0n1", 3);
    ASSERT_STR_EQ(out, "nvme0n1p3");
    return 0;
}

TestCase blkdev_tests[] = {
    TEST_ENTRY(blk_register_scans_gpt),
    TEST_ENTRY(blk_partition_offset_and_bounds),
    TEST_ENTRY(blk_many_in_flight_one_kick),
    TEST_ENTRY(blk_depth_limit_and_out_of_order_tags),
    TEST_ENTRY(blk_plug_merges_adjacent_within_limits),
    TEST_ENTRY(blk_merge_into_pending_tail),
    TEST_ENTRY(blk_device_busy_requeues_in_order),
    TEST_ENTRY(blk_sync_splits_unaligned_buffer),
    TEST_ENTRY(blk_one_segment_bounces_straddling_blocks),
    TEST_ENTRY(blk_4kn_requests_cut_on_block_boundaries),
    TEST_ENTRY(blk_refusals_and_flush),
    TEST_ENTRY(blk_submit_page_async),
    TEST_ENTRY(blk_lost_interrupt_completed_by_poll),
    TEST_ENTRY(blk_hung_device_reset_fails_everything),
    TEST_ENTRY(blk_names),
};

int blkdev_test_count = sizeof(blkdev_tests) / sizeof(blkdev_tests[0]);
