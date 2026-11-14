/* arc_os — The block layer's API over memory, for the VFS host suites (M1-10)
 *
 * fat32.c and filemap.c reach disks only through drivers/blkdev.h. Here a
 * BlockDevice is memory: driver_data points at the image, nr_sectors is its
 * size. Every request runs at once and ends in the submitting thread, one of
 * the cases the real layer has too (blkdev.h); the interrupt path is the QEMU
 * suites' job, and blkdev.c itself has its own host suite (test_blkdev.c).
 * blk_mem_fail makes every request fail with -EIO; blk_mem_requests counts
 * the requests that reached the "device". With blk_mem_defer, requests wait
 * until blk_mem_run (as if their interrupts came later) and a read then fills
 * its bytes with BLK_MEM_POISON, marking what an async read wrote. A write's
 * bytes are copied when it ends, as a DMA engine reading the page late would
 * (a page changed while its write is in flight shows). The write-order
 * audit is in blk_mem.h. */

#include <stdlib.h>
#include <string.h>
#include "drivers/blkdev.h"
#include "mm/page.h"
#include "mm/filemap.h"
#include <stdio.h>
#include "uapi/errno.h"
#include "blk_mem.h"

int blk_mem_fail;
int blk_mem_requests;
int blk_mem_flushes;
int blk_mem_defer;
static BlkRequest *deferred;   /* LIFO; order does not matter here */
static BlockDevice **mem_devs;   /* the registry stand-in (blk_mem_devices) */
static unsigned mem_ndevs;

int (*blk_mem_classify)(const BlockDevice *dev, uint64_t sector, int file_page);
unsigned blk_mem_before[BLK_MEM_CLASSES];
int blk_mem_audit;
int blk_mem_order_errors;
char blk_mem_order_error[200];

/* Per device: writes of each class in flight, and done but not flushed */
typedef struct {
    const BlockDevice *dev;
    int pending[BLK_MEM_CLASSES];
    int unflushed[BLK_MEM_CLASSES];
} OrderState;
static OrderState order[16];

static OrderState *order_of(const BlockDevice *dev) {
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (order[i].dev == dev) return &order[i];
        if (order[i].dev == NULL) {
            order[i].dev = dev;
            return &order[i];
        }
    }
    return NULL;   /* more devices than the audit follows: not counted */
}

/* The class of a write (< 0: none) */
static int order_class(const BlockDevice *dev, uint64_t sector, struct page *pg) {
    if (blk_mem_classify == NULL) return -1;
    int file_page = pg != NULL && pg->mapping != NULL && ((AddressSpace *)pg->mapping)->host != NULL;
    int c = blk_mem_classify(dev, sector, file_page);
    return c < BLK_MEM_CLASSES ? c : -1;
}

/* A write starts: checked against the classes that must be on the medium first */
static void order_start(const BlockDevice *dev, int c, uint64_t sector) {
    OrderState *st = c >= 0 ? order_of(dev) : NULL;
    if (st == NULL) return;
    for (int l = 0; blk_mem_audit && l < BLK_MEM_CLASSES; l++) {
        if (!(blk_mem_before[c] & (1u << l)) || st->pending[l] + st->unflushed[l] == 0) continue;
        if (blk_mem_order_errors++ == 0)
            snprintf(blk_mem_order_error, sizeof(blk_mem_order_error),
                     "class %d write at sector %llu with %d class-%d writes in flight and %d not flushed", c,
                     (unsigned long long)sector, st->pending[l], l, st->unflushed[l]);
    }
    st->pending[c]++;
}

static void order_end(const BlockDevice *dev, int c) {
    OrderState *st = c >= 0 ? order_of(dev) : NULL;
    if (st == NULL) return;
    st->pending[c]--;
    if (dev->flags & BLK_CACHE) st->unflushed[c]++;   /* else on the medium once done */
}

static void order_flush(const BlockDevice *dev) {
    OrderState *st = blk_mem_classify != NULL ? order_of(dev) : NULL;
    if (st != NULL) memset(st->unflushed, 0, sizeof(st->unflushed));
}

void blk_mem_devices(BlockDevice **devs, unsigned n) {
    mem_devs = devs;
    mem_ndevs = n;
}

BlockDevice *blk_get(unsigned index) {
    return index < mem_ndevs ? mem_devs[index] : NULL;
}

/* bytes at 512-byte sector of dev, to or from buf. 0 or -errno. */
static int mem_io(BlockDevice *dev, int op, uint64_t sector, uint8_t *buf, size_t bytes) {
    if (op == BLK_OP_FLUSH) {
        blk_mem_flushes++;
        order_flush(dev);   /* what completed before it is on the medium */
        return 0;
    }
    if (bytes == 0 || bytes % BLK_SECTOR_SIZE != 0 || sector > dev->nr_sectors ||
        bytes / BLK_SECTOR_SIZE > dev->nr_sectors - sector) {
        return -EINVAL;
    }
    blk_mem_requests++;
    if (blk_mem_fail) return -EIO;
    uint8_t *disk = (uint8_t *)dev->driver_data + sector * BLK_SECTOR_SIZE;
    if (op == BLK_OP_READ)
        memcpy(buf, disk, bytes);
    else if (op == BLK_OP_WRITE && !(dev->flags & BLK_RDONLY))
        memcpy(disk, buf, bytes);
    else
        return op == BLK_OP_WRITE ? -EROFS : -EINVAL;
    return 0;
}

BlkRequest *blk_rq_alloc(BlockDevice *dev, int op, uint64_t sector, BlkEndIo end_io, void *priv) {
    BlkRequest *rq = calloc(1, sizeof(BlkRequest));
    if (rq == NULL) return NULL;
    rq->disk = dev;
    rq->op = (uint8_t)op;
    rq->sector = sector;
    rq->end_io = end_io;
    rq->priv = priv;
    return rq;
}

void blk_rq_free(BlkRequest *rq) {
    free(rq);
}

int blk_rq_add_page(BlkRequest *rq, struct page *pg, uint32_t offset, uint32_t len) {
    if (rq->nsegs >= BLK_RQ_SEGS) return -E2BIG;
    rq->seg[rq->nsegs].page = pg;
    rq->seg[rq->nsegs].offset = offset;
    rq->seg[rq->nsegs].len = len;
    rq->nsegs++;
    rq->bytes += len;
    return 0;
}

static void run_one(BlkRequest *rq, int poison);

/* A request's write class, from its first page (no merging here: one
 * request is one mapping's run) */
static int rq_class(const BlkRequest *rq) {
    if (rq->op != BLK_OP_WRITE || rq->nsegs == 0) return -1;
    return order_class(rq->disk, rq->sector, rq->seg[0].page);
}

void blk_submit(BlkRequest *rq) {
    order_start(rq->disk, rq_class(rq), rq->sector);
    if (blk_mem_defer) {
        rq->next = deferred;
        deferred = rq;
        return;
    }
    run_one(rq, 0);
}

void blk_mem_run(void) {
    while (deferred != NULL) {
        BlkRequest *rq = deferred;
        deferred = rq->next;
        run_one(rq, 1);
    }
}

static void run_one(BlkRequest *rq, int poison) {
    int status = 0;
    uint64_t sector = rq->sector;
    for (uint32_t i = 0; i < rq->nsegs && status == 0; i++) {
        BlkSeg *s = &rq->seg[i];
        uint8_t *buf = (uint8_t *)page_to_virt(s->page) + s->offset;
        status = mem_io(rq->disk, rq->op, sector, buf, s->len);
        if (status == 0 && poison && rq->op == BLK_OP_READ) memset(buf, BLK_MEM_POISON, s->len);
        sector += s->len / BLK_SECTOR_SIZE;
    }
    if (rq->nsegs == 0) status = mem_io(rq->disk, rq->op, sector, NULL, 0);
    order_end(rq->disk, rq_class(rq));
    rq->end_io(rq->priv, status);
    free(rq);
}

void blk_plug_init(BlkPlug *plug) {
    plug->head = plug->tail = NULL;
}

void blk_plug_add(BlkPlug *plug, BlkRequest *rq) {
    rq->next = NULL;
    if (plug->tail != NULL)
        plug->tail->next = rq;
    else
        plug->head = rq;
    plug->tail = rq;
}

void blk_plug_flush(BlkPlug *plug) {
    BlkRequest *rq = plug->head;
    plug->head = plug->tail = NULL;
    while (rq != NULL) {
        BlkRequest *next = rq->next;
        blk_submit(rq);
        rq = next;
    }
}

void blk_poll_all(void) {
    /* every request has ended by the time its submit returns */
}

int blk_rw_sync(BlockDevice *dev, int op, uint64_t sector, void *buf, size_t bytes) {
    int c = op == BLK_OP_WRITE ? order_class(dev, sector, NULL) : -1;
    order_start(dev, c, sector);
    int err = mem_io(dev, op, sector, buf, bytes);
    order_end(dev, c);
    return err;
}
