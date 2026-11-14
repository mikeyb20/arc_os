#ifndef ARCHOS_DRIVERS_BLKDEV_H
#define ARCHOS_DRIVERS_BLKDEV_H

/* Block layer (M1-10, ADR-0005, ADR-0008): asynchronous requests with many in
 * flight and interrupt completion, a queue per disk with a depth limit,
 * plugging and merging, GPT partitions as devices, and lookups by name,
 * label and type GUID.
 *
 * Units: sectors are always 512 bytes, whatever the device's logical block
 * size (as in Linux); a request's start and length must be multiples of the
 * logical block size. A request carries pages, never copies: each segment is
 * (page, offset, length) inside one page block.
 *
 * Lifetime: a request belongs to the block layer from blk_submit (or
 * blk_plug_add) on. Its end_io runs exactly once, then the request is freed.
 * end_io runs in interrupt context (or in the submitter, for a request that
 * fails its checks): it must not sleep or allocate; it may wake waiters and
 * unlock pages. Filesystems and the page cache use these calls only; nothing
 * outside drivers calls a driver's functions directly. */

#include <stddef.h>
#include <stdint.h>
#include "drivers/gpt.h"
#include "proc/spinlock.h"

struct page;
typedef struct BlockDevice BlockDevice;
typedef struct BlkRequest BlkRequest;

#define BLK_SECTOR_SIZE  512
#define BLK_SECTOR_SHIFT 9

/* Operations */
#define BLK_OP_READ  0
#define BLK_OP_WRITE 1
#define BLK_OP_FLUSH 2   /* write out the volatile cache: writes completed before it (fsync); no data */

/* Segments one request carries (64 KiB of 4 KiB pages); merged requests
 * together may carry up to the device's max_segments */
#define BLK_RQ_SEGS 16

/* Device flags */
#define BLK_RDONLY 0x1   /* writes fail with -EROFS */
#define BLK_CACHE  0x2   /* volatile write cache: BLK_OP_FLUSH reaches the device */

/* Registry size: disks plus partitions, and disks alone */
#define BLK_MAX_DEVICES 64
#define BLK_MAX_DISKS   16
/* Requests a queue keeps in flight at most (tags) */
#define BLK_MAX_DEPTH   128
/* A request not completed after this long is polled, then the device is
 * reset and everything in flight fails (time-bounded completion) */
#define BLK_TIMEOUT_MS  30000

typedef void (*BlkEndIo)(void *priv, int status);

typedef struct {
    struct page *page;
    uint32_t offset;   /* inside the page block */
    uint32_t len;
} BlkSeg;

struct BlkRequest {
    BlockDevice *disk;   /* whole disk (partitions are remapped at submit) */
    uint64_t sector;     /* disk sector */
    uint32_t bytes;      /* this request's segments */
    uint8_t op;
    uint8_t nsegs;
    uint16_t tag;        /* 0..depth-1 while dispatched (drivers index per-tag data with it) */
    int status;
    BlkEndIo end_io;
    void *priv;
    BlkRequest *next, *prev;   /* queue lists */
    /* Merging: a head request carries followers (adjacent, same op) */
    BlkRequest *merge_next;
    BlkRequest *merge_tail;    /* head only */
    uint32_t total_bytes;      /* head only: the whole chain */
    uint16_t total_segs;
    uint64_t deadline;         /* TSC, set at dispatch */
    BlkSeg seg[BLK_RQ_SEGS];
};

/* Walk every segment of a (possibly merged) request: r and i are the loop's
 * request and segment index. */
#define blk_rq_for_each_seg(head, r, i)                    \
    for ((r) = (head); (r) != NULL; (r) = (r)->merge_next) \
        for ((i) = 0; (i) < (r)->nsegs; (i)++)

/* What a driver implements. Each op runs with the queue lock held and
 * interrupts off, so it must not sleep or allocate. */
typedef struct {
    /* Start rq on the device (without the doorbell). Returns 0, BLK_BUSY if
     * the device has no room (rq waits for a completion), or -errno to fail
     * rq. */
    int (*queue_rq)(BlockDevice *disk, BlkRequest *rq);
    /* Ring the doorbell once for the requests just started. */
    void (*kick)(BlockDevice *disk);
    /* Reap finished requests: call blk_rq_done for each. From the interrupt
     * path (blk_complete) and from the timeout path. */
    void (*poll)(BlockDevice *disk);
    /* A request timed out after poll found nothing: stop the device so it
     * writes no memory any more (reset), and forget every request it had.
     * The block layer then fails them and every later one with -EIO. */
    void (*reset)(BlockDevice *disk);
} BlockOps;

#define BLK_BUSY 1

typedef struct {
    uint64_t submitted;   /* requests accepted */
    uint64_t merged;      /* of those, merged behind another */
    uint64_t dispatched;  /* driver requests started */
    uint64_t completed;   /* requests ended (any status) */
    uint64_t errors;
    uint64_t polled;      /* completed by the timeout path's poll (no interrupt came) */
    uint64_t timeouts;    /* device resets */
    uint32_t in_flight;
    uint32_t max_in_flight;
} BlkStats;

typedef struct BlkQueue {
    Spinlock lock;   /* LOCK_LEVEL_BLK_QUEUE, IRQ-safe: everything below */
    uint32_t depth;
    uint32_t in_flight;
    BlkRequest *pend_head, *pend_tail;   /* accepted, not dispatched: FIFO */
    BlkRequest *fly_head, *fly_tail;     /* dispatched, oldest first */
    BlkRequest *done;                    /* finished, end_io not run yet */
    BlkRequest **tags;                   /* tag -> request */
    uint16_t *free_tags;
    uint32_t nfree_tags;
    uint64_t timeout_cycles;
    int dead;
    uint32_t drop_irqs;   /* test mode: interrupts to ignore (completion by timeout poll) */
    BlkStats stats;
} BlkQueue;

struct BlockDevice {
    char name[16];             /* "vda", "vda1" */
    BlockDevice *disk;         /* the whole disk (itself for a disk) */
    uint64_t start;            /* first disk sector (partitions; 0 for a disk) */
    uint64_t nr_sectors;       /* capacity in 512-byte sectors */
    uint32_t logical_block;    /* 512 or 4096 */
    uint32_t physical_block;
    uint32_t max_segments;     /* per driver request, merged chain included */
    uint32_t max_transfer;     /* bytes per driver request */
    uint32_t flags;            /* BLK_RDONLY, BLK_CACHE */
    /* GPT (disks with a table, and their partitions) */
    int has_gpt;
    Guid disk_guid;            /* the disk's GPT GUID */
    Guid part_guid;            /* partitions: PARTUUID */
    Guid type_guid;            /* partitions: type */
    uint32_t partno;           /* partitions: 1-based; 0 for a disk */
    char label[GPT_LABEL_MAX]; /* partitions: PARTLABEL */
    /* Disks only */
    const BlockOps *ops;
    BlkQueue *queue;
    void *driver_data;
};

/* --- Drivers --- */

/* Register a disk the driver filled in (nr_sectors, logical_block,
 * physical_block, max_segments, max_transfer, flags, ops, driver_data),
 * with depth requests in flight at most. Names it prefix + letter ("vd" ->
 * "vda") unless disk->name is already set, reads its GPT and registers the
 * partitions. Thread context; reads
 * the disk, polling if the scheduler can't sleep yet. Returns 0 or -errno. */
int blk_disk_register(BlockDevice *disk, const char *prefix, uint32_t depth);

/* Interrupt path: reap finished requests (ops->poll), start waiting ones and
 * run end_io for what finished. Any context. */
void blk_complete(BlockDevice *disk);

/* From ops->poll or ops->reset only (queue lock held): rq has finished with
 * status (0 or -errno). */
void blk_rq_done(BlkRequest *rq, int status);

/* Timeout check of every disk, at time now (TSC). Called from the timer
 * tick until M1-06's timer queue arms a one-shot per disk instead. O(1)
 * while nothing is in flight. Any context. */
void blk_expire_all(uint64_t now);
void blk_expire(BlockDevice *disk, uint64_t now);

/* --- Users: filesystems, page cache --- */

/* A request for op at 512-byte sector of dev (a disk or partition), no
 * segments yet. Thread context (allocates). NULL if out of memory. */
BlkRequest *blk_rq_alloc(BlockDevice *dev, int op, uint64_t sector, BlkEndIo end_io, void *priv);

/* Free a request that was never submitted. */
void blk_rq_free(BlkRequest *rq);

/* Add len bytes at offset in page pg. Returns 0, or -E2BIG when the request
 * is full (submit it and start another). */
int blk_rq_add_page(BlkRequest *rq, struct page *pg, uint32_t offset, uint32_t len);

/* Submit one request (O(1)). end_io always runs, with -EINVAL, -EIO or
 * -EROFS at once if the request fails its checks. */
void blk_submit(BlkRequest *rq);

/* Plugging: collect requests, merging adjacent ones, then hand them all to
 * the device at once (one lock round, one doorbell). On the stack. */
typedef struct {
    BlkRequest *head, *tail;
} BlkPlug;

void blk_plug_init(BlkPlug *plug);
void blk_plug_add(BlkPlug *plug, BlkRequest *rq);
void blk_plug_flush(BlkPlug *plug);

/* One-call async page I/O (M1-12's readpage): op on len bytes at offset in
 * pg, from sector of dev. end_io(priv, status) runs once, also on failure.
 * Thread context. Returns 0, or -ENOMEM (then end_io does not run). */
int blk_submit_page(BlockDevice *dev, int op, uint64_t sector, struct page *pg, uint32_t offset, uint32_t len,
                    BlkEndIo end_io, void *priv);

/* Synchronous I/O on a kernel buffer (any kernel address), split into
 * requests the device takes. Waits for all of them: sleeps, or before the
 * scheduler runs, polls the device. Thread context. Returns 0 or -errno. */
int blk_rw_sync(BlockDevice *dev, int op, uint64_t sector, void *buf, size_t bytes);

/* --- Lookups (mount time; O(devices)) --- */

/* The index-th registered device (disks and partitions, in order), or NULL. */
BlockDevice *blk_get(unsigned index);
/* By name ("vda", "vda1"), or NULL. */
BlockDevice *blk_find(const char *name);
/* The first partition with this type GUID on the disk with this GPT GUID. */
BlockDevice *blk_find_part(const Guid *disk_guid, const Guid *type);
/* The first partition with this label. */
BlockDevice *blk_find_label(const char *label);

/* Statistics of dev's disk queue. */
void blk_get_stats(BlockDevice *dev, BlkStats *out);

/* Test mode (diag/blktest.c): set the request timeout of dev's disk and
 * how many of its next interrupts to ignore. */
void blk_test_set(BlockDevice *dev, uint32_t timeout_ms, uint32_t drop_irqs);

#endif /* ARCHOS_DRIVERS_BLKDEV_H */
