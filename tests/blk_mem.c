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
 * its bytes with BLK_MEM_POISON, marking what an async read wrote. */

#include <stdlib.h>
#include <string.h>
#include "drivers/blkdev.h"
#include "mm/page.h"
#include "uapi/errno.h"
#include "blk_mem.h"

int blk_mem_fail;
int blk_mem_requests;
int blk_mem_defer;
static BlkRequest *deferred;   /* LIFO; order does not matter here */

/* bytes at 512-byte sector of dev, to or from buf. 0 or -errno. */
static int mem_io(BlockDevice *dev, int op, uint64_t sector, uint8_t *buf, size_t bytes) {
    if (op == BLK_OP_FLUSH) return 0;
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

void blk_submit(BlkRequest *rq) {
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
    return mem_io(dev, op, sector, buf, bytes);
}
