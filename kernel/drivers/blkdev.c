/* arc_os — Block layer (M1-10, ADR-0005)
 *
 * Owns: the registry of block devices (disks and their GPT partitions), one
 * request queue per disk, the request slab cache.
 * Invariants: a request is on exactly one of: a plug, a queue's pending
 * FIFO, its flight list (with a tag), its done list, or nowhere (end_io run,
 * freed). Requests leave the flight list only through blk_rq_done (or a
 * BLK_BUSY dispatch, back to the pending head). The flight list is in
 * dispatch order, and every request of a queue gets the same timeout, so
 * its head has the earliest deadline.
 * end_io runs with no block-layer lock held. Registry entries are published
 * (written, then the count raised) and never removed.
 * Locks: BlkQueue.lock (LOCK_LEVEL_BLK_QUEUE, IRQ-safe) around everything in
 * the queue and every BlockOps call; the registry lock (LOCK_LEVEL_BLK_REGISTRY)
 * only to add devices. Lookups read the published entries without it.
 * Cost: submit O(1) with one merge check against the tail; completion O(1)
 * per request plus end_io; nothing is allocated in interrupt context
 * (requests come from a slab cache at submit and are freed after end_io).
 * The timeout tick is O(1) while nothing is in flight, else O(disks) (at
 * most BLK_MAX_DISKS) until M1-06 arms one timer per disk. */

#include "drivers/blkdev.h"
#include "mm/slab.h"
#include "mm/kmalloc.h"
#include "mm/page.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "proc/semaphore.h"
#include "proc/sched.h"
#include "arch/hal.h"
#include "diag/diag.h"   /* diag_ms_to_cycles until M1-06's clocksource */
#include "lib/kprintf.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "uapi/errno.h"

#define GPT_SCAN_MAX 32   /* partitions registered per disk */

static BlockDevice *devices[BLK_MAX_DEVICES];
static uint32_t ndevices;   /* published count (acquire/release) */
static BlockDevice *disks[BLK_MAX_DISKS];   /* the whole disks, for the timeout tick */
static uint32_t ndisks;
static Spinlock registry_lock = SPINLOCK_INIT(LOCK_LEVEL_BLK_REGISTRY, "block devices");
static struct kmem_cache rq_cache_store;
static struct kmem_cache *rq_cache;
static uint32_t inflight_total;   /* every queue's in_flight: blk_expire_all's fast exit */

/* --- Registry --- */

static int registry_add(BlockDevice *dev) {
    uint64_t flags;
    int rc = -ENOSPC;
    spin_lock_irqsave(&registry_lock, &flags);
    uint32_t n = ndevices, nd = ndisks;
    if (n < BLK_MAX_DEVICES && (dev->partno != 0 || nd < BLK_MAX_DISKS)) {
        devices[n] = dev;
        __atomic_store_n(&ndevices, n + 1, __ATOMIC_RELEASE);
        if (dev->partno == 0) {
            disks[nd] = dev;
            __atomic_store_n(&ndisks, nd + 1, __ATOMIC_RELEASE);
        }
        rc = 0;
    }
    spin_unlock_irqrestore(&registry_lock, flags);
    return rc;
}

BlockDevice *blk_get(unsigned index) {
    return index < __atomic_load_n(&ndevices, __ATOMIC_ACQUIRE) ? devices[index] : NULL;
}

BlockDevice *blk_find(const char *name) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

BlockDevice *blk_find_part(const Guid *disk_guid, const Guid *type) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno != 0 && gpt_guid_equal(&d->disk_guid, disk_guid) && gpt_guid_equal(&d->type_guid, type)) return d;
    }
    return NULL;
}

BlockDevice *blk_find_label(const char *label) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno != 0 && strcmp(d->label, label) == 0) return d;
    }
    return NULL;
}

void blk_get_stats(BlockDevice *dev, BlkStats *out) {
    BlkQueue *q = dev->disk->queue;
    uint64_t flags;
    spin_lock_irqsave(&q->lock, &flags);
    *out = q->stats;
    out->in_flight = q->in_flight;
    spin_unlock_irqrestore(&q->lock, flags);
}

void blk_test_set(BlockDevice *dev, uint32_t timeout_ms, uint32_t drop_irqs) {
    BlkQueue *q = dev->disk->queue;
    uint64_t flags;
    spin_lock_irqsave(&q->lock, &flags);
    q->timeout_cycles = diag_ms_to_cycles(timeout_ms);
    q->drop_irqs = drop_irqs;
    spin_unlock_irqrestore(&q->lock, flags);
}

/* --- Queue lists (queue lock held) --- */

static void list_append(BlkRequest **head, BlkRequest **tail, BlkRequest *rq) {
    rq->next = NULL;
    rq->prev = *tail;
    if (*tail)
        (*tail)->next = rq;
    else
        *head = rq;
    *tail = rq;
}

static void list_remove(BlkRequest **head, BlkRequest **tail, BlkRequest *rq) {
    if (rq->prev)
        rq->prev->next = rq->next;
    else
        *head = rq->next;
    if (rq->next)
        rq->next->prev = rq->prev;
    else
        *tail = rq->prev;
    rq->next = rq->prev = NULL;
}

static void list_push_front(BlkRequest **head, BlkRequest **tail, BlkRequest *rq) {
    rq->prev = NULL;
    rq->next = *head;
    if (*head)
        (*head)->prev = rq;
    else
        *tail = rq;
    *head = rq;
}

static uint32_t chain_len(const BlkRequest *head) {
    uint32_t n = 0;
    for (; head != NULL; head = head->merge_next) n++;
    return n;
}

/* Finished head: off to the done list (queue lock held) */
static void mark_done(BlkQueue *q, BlkRequest *rq, int status) {
    rq->status = status;
    uint32_t n = chain_len(rq);
    q->stats.completed += n;
    if (status != 0) q->stats.errors += n;
    rq->next = q->done;
    q->done = rq;
}

void blk_rq_done(BlkRequest *rq, int status) {
    BlkQueue *q = rq->disk->queue;
    list_remove(&q->fly_head, &q->fly_tail, rq);
    q->tags[rq->tag] = NULL;
    q->free_tags[q->nfree_tags++] = rq->tag;
    q->in_flight--;
    __atomic_sub_fetch(&inflight_total, 1, __ATOMIC_RELAXED);
    mark_done(q, rq, status);
}

/* Start pending requests while tags are free (queue lock held). */
static void dispatch(BlockDevice *disk, BlkQueue *q) {
    int started = 0;
    while (q->pend_head != NULL && q->nfree_tags > 0 && !q->dead) {
        BlkRequest *rq = q->pend_head;
        list_remove(&q->pend_head, &q->pend_tail, rq);
        rq->tag = q->free_tags[--q->nfree_tags];
        q->tags[rq->tag] = rq;
        rq->deadline = hal_tsc_read() + q->timeout_cycles;
        list_append(&q->fly_head, &q->fly_tail, rq);
        q->in_flight++;
        __atomic_add_fetch(&inflight_total, 1, __ATOMIC_RELAXED);
        int rc = disk->ops->queue_rq(disk, rq);
        if (rc == 0) {
            started = 1;
            q->stats.dispatched++;
            if (q->in_flight > q->stats.max_in_flight) q->stats.max_in_flight = q->in_flight;
        } else if (rc == BLK_BUSY) {   /* no room in the device: wait for a completion */
            list_remove(&q->fly_head, &q->fly_tail, rq);
            q->tags[rq->tag] = NULL;
            q->free_tags[q->nfree_tags++] = rq->tag;
            q->in_flight--;
            __atomic_sub_fetch(&inflight_total, 1, __ATOMIC_RELAXED);
            list_push_front(&q->pend_head, &q->pend_tail, rq);
            break;
        } else {
            blk_rq_done(rq, rc);
        }
    }
    if (started && disk->ops->kick) disk->ops->kick(disk);
}

/* Take the done list (queue lock held); run it with finish_done after unlocking */
static BlkRequest *take_done(BlkQueue *q) {
    BlkRequest *d = q->done;
    q->done = NULL;
    return d;
}

static void finish_done(BlkRequest *d) {
    while (d != NULL) {
        BlkRequest *next_head = d->next;
        int status = d->status;
        for (BlkRequest *r = d; r != NULL;) {
            BlkRequest *nr = r->merge_next;
            r->end_io(r->priv, status);
            kmem_cache_free(rq_cache, r);
            r = nr;
        }
        d = next_head;
    }
}

/* --- Completion and timeouts --- */

void blk_complete(BlockDevice *disk) {
    BlkQueue *q = disk->queue;
    uint64_t flags;
    spin_lock_irqsave(&q->lock, &flags);
    if (q->drop_irqs > 0) {   /* test mode: as if the interrupt never came */
        q->drop_irqs--;
        spin_unlock_irqrestore(&q->lock, flags);
        return;
    }
    disk->ops->poll(disk);
    dispatch(disk, q);
    BlkRequest *d = take_done(q);
    spin_unlock_irqrestore(&q->lock, flags);
    finish_done(d);
}

/* Fail everything the queue holds with -EIO (queue lock held, device reset) */
static void fail_all(BlkQueue *q) {
    while (q->fly_head != NULL) blk_rq_done(q->fly_head, -EIO);
    while (q->pend_head != NULL) {
        BlkRequest *rq = q->pend_head;
        list_remove(&q->pend_head, &q->pend_tail, rq);
        mark_done(q, rq, -EIO);
    }
}

void blk_expire(BlockDevice *disk, uint64_t now) {
    BlkQueue *q = disk->queue;
    if (__atomic_load_n(&q->in_flight, __ATOMIC_RELAXED) == 0) return;
    uint64_t flags;
    spin_lock_irqsave(&q->lock, &flags);
    if (q->fly_head != NULL && now >= q->fly_head->deadline) {
        /* First ask the device: the work may be done and only the interrupt lost */
        uint64_t before = q->stats.completed;
        disk->ops->poll(disk);
        if (q->stats.completed != before) {
            if (q->stats.polled == 0) kprintf("[BLK] %s: completions without an interrupt (polled)\n", disk->name);
            q->stats.polled += q->stats.completed - before;
        }
        if (q->fly_head != NULL && now >= q->fly_head->deadline) {
            kprintf("[BLK] %s: request at sector %lu timed out; resetting the device, failing %u requests\n",
                    disk->name, q->fly_head->sector, q->in_flight);
            q->stats.timeouts++;
            disk->ops->reset(disk);
            q->dead = 1;
            fail_all(q);
        }
        dispatch(disk, q);
    }
    BlkRequest *d = take_done(q);
    spin_unlock_irqrestore(&q->lock, flags);
    finish_done(d);
}

void blk_expire_all(uint64_t now) {
    if (__atomic_load_n(&inflight_total, __ATOMIC_RELAXED) == 0) return;
    uint32_t n = __atomic_load_n(&ndisks, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) blk_expire(disks[i], now);
}

void blk_poll_all(void) {
    if (__atomic_load_n(&inflight_total, __ATOMIC_RELAXED) == 0) return;
    uint64_t now = hal_tsc_read();
    uint32_t n = __atomic_load_n(&ndisks, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        if (__atomic_load_n(&disks[i]->queue->in_flight, __ATOMIC_RELAXED) == 0) continue;
        blk_complete(disks[i]);
        blk_expire(disks[i], now);
    }
}

/* --- Requests --- */

BlkRequest *blk_rq_alloc(BlockDevice *dev, int op, uint64_t sector, BlkEndIo end_io, void *priv) {
    BlkRequest *rq = kmem_cache_alloc(rq_cache, 0);
    if (rq == NULL) return NULL;
    memset(rq, 0, offsetof(BlkRequest, seg));
    rq->disk = dev;   /* remapped to the whole disk at submit */
    rq->op = (uint8_t)op;
    rq->sector = sector;
    rq->end_io = end_io;
    rq->priv = priv;
    return rq;
}

void blk_rq_free(BlkRequest *rq) {
    kmem_cache_free(rq_cache, rq);
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

/* Check rq against the device it was made for and remap it to the disk.
 * Returns 0 or -errno. */
static int rq_prepare(BlkRequest *rq) {
    BlockDevice *dev = rq->disk;
    BlockDevice *disk = dev->disk;
    if (rq->op == BLK_OP_FLUSH) {
        if (rq->nsegs != 0) return -EINVAL;
    } else {
        uint64_t lbs = disk->logical_block;
        uint64_t nsect = rq->bytes >> BLK_SECTOR_SHIFT;
        if (rq->op != BLK_OP_READ && rq->op != BLK_OP_WRITE) return -EINVAL;
        if (rq->bytes == 0 || rq->bytes % lbs != 0 || ((rq->sector << BLK_SECTOR_SHIFT) % lbs) != 0) return -EINVAL;
        if (rq->sector >= dev->nr_sectors || nsect > dev->nr_sectors - rq->sector) return -EINVAL;
        if (rq->op == BLK_OP_WRITE && (dev->flags & BLK_RDONLY)) return -EROFS;
        if (rq->nsegs > disk->max_segments || rq->bytes > disk->max_transfer) return -EINVAL;
        for (uint32_t i = 0; i < rq->nsegs; i++) {
            if (rq->seg[i].len == 0 || ((rq->seg[i].offset | rq->seg[i].len) & 3) != 0) return -EINVAL;
        }
    }
    rq->sector += dev->start;
    rq->disk = disk;
    rq->merge_tail = rq;
    rq->total_bytes = rq->bytes;
    rq->total_segs = rq->nsegs;
    return 0;
}

/* 1 if chain b can follow chain a in one device request */
static int can_merge(const BlkRequest *a, const BlkRequest *b) {
    return a->disk == b->disk && a->op == b->op && a->op != BLK_OP_FLUSH &&
           a->sector + (a->total_bytes >> BLK_SECTOR_SHIFT) == b->sector &&
           (uint32_t)a->total_segs + b->total_segs <= a->disk->max_segments &&
           (uint64_t)a->total_bytes + b->total_bytes <= a->disk->max_transfer;
}

static void do_merge(BlkRequest *a, BlkRequest *b) {
    a->merge_tail->merge_next = b;
    a->merge_tail = b->merge_tail;
    a->total_bytes += b->total_bytes;
    a->total_segs = (uint16_t)(a->total_segs + b->total_segs);
}

void blk_plug_init(BlkPlug *plug) {
    plug->head = plug->tail = NULL;
}

void blk_plug_add(BlkPlug *plug, BlkRequest *rq) {
    int rc = rq_prepare(rq);
    if (rc != 0) {
        rq->end_io(rq->priv, rc);
        kmem_cache_free(rq_cache, rq);
        return;
    }
    if (plug->tail != NULL && can_merge(plug->tail, rq)) {
        do_merge(plug->tail, rq);
        return;
    }
    rq->next = NULL;
    if (plug->tail)
        plug->tail->next = rq;
    else
        plug->head = rq;
    plug->tail = rq;
}

void blk_plug_flush(BlkPlug *plug) {
    BlkRequest *rq = plug->head;
    plug->head = plug->tail = NULL;
    while (rq != NULL) {
        BlockDevice *disk = rq->disk;
        BlkQueue *q = disk->queue;
        uint64_t flags;
        spin_lock_irqsave(&q->lock, &flags);
        /* Every head of the plug bound for this disk, under one lock round */
        while (rq != NULL && rq->disk == disk) {
            BlkRequest *next = rq->next;
            uint32_t n = chain_len(rq);
            q->stats.submitted += n;
            q->stats.merged += n - 1;
            if (q->dead) {
                mark_done(q, rq, -EIO);
            } else if (rq->op == BLK_OP_FLUSH && !(disk->flags & BLK_CACHE)) {
                mark_done(q, rq, 0);   /* no volatile cache: nothing to flush */
            } else if (q->pend_tail != NULL && can_merge(q->pend_tail, rq)) {
                do_merge(q->pend_tail, rq);
                q->stats.merged++;
            } else {
                list_append(&q->pend_head, &q->pend_tail, rq);
            }
            rq = next;
        }
        dispatch(disk, q);
        BlkRequest *d = take_done(q);
        spin_unlock_irqrestore(&q->lock, flags);
        finish_done(d);
    }
}

void blk_submit(BlkRequest *rq) {
    BlkPlug plug;
    blk_plug_init(&plug);
    blk_plug_add(&plug, rq);
    blk_plug_flush(&plug);
}

int blk_submit_page(BlockDevice *dev, int op, uint64_t sector, struct page *pg, uint32_t offset, uint32_t len,
                    BlkEndIo end_io, void *priv) {
    BlkRequest *rq = blk_rq_alloc(dev, op, sector, end_io, priv);
    if (rq == NULL) return -ENOMEM;
    blk_rq_add_page(rq, pg, offset, len);
    blk_submit(rq);
    return 0;
}

/* --- Synchronous I/O --- */

typedef struct {
    Semaphore done;   /* one post per request */
    int status;       /* first error */
} SyncWait;

static void sync_end_io(void *priv, int status) {
    SyncWait *w = priv;
    if (status != 0 && w->status == 0) w->status = status;
    sem_post(&w->done);   /* last touch of w: the waiter may return at once */
}

/* The page under kernel address va, or NULL */
static struct page *kva_page(uintptr_t va) {
    uint64_t phys = vmm_get_phys(va & ~(uintptr_t)(PAGE_SIZE - 1));
    if (phys == 0 || !pfn_valid(phys >> PAGE_SHIFT)) return NULL;
    return pfn_to_page(phys >> PAGE_SHIFT);
}

/* Fill rq with up to the device's limits from buf + pos (bytes left: left).
 * Cuts the request to whole logical blocks unless it ends the buffer.
 * Returns the bytes taken, 0 when not even one block fits (a block straddles
 * pages and the device takes one segment: the caller bounces it), or -errno. */
static int64_t fill_request(BlkRequest *rq, uintptr_t va, uint64_t left) {
    BlockDevice *disk = rq->disk->disk;
    uint32_t max_segs = disk->max_segments < BLK_RQ_SEGS ? disk->max_segments : BLK_RQ_SEGS;
    uint32_t max_bytes = disk->max_transfer - disk->max_transfer % disk->logical_block;
    uint32_t taken = 0;
    while (taken < left && rq->nsegs < max_segs && taken < max_bytes) {
        uintptr_t a = va + taken;
        uint32_t off = (uint32_t)(a & (PAGE_SIZE - 1));
        uint64_t len = PAGE_SIZE - off;
        if (len > left - taken) len = left - taken;
        if (len > max_bytes - taken) len = max_bytes - taken;
        struct page *pg = kva_page(a);
        if (pg == NULL) return -EFAULT;
        blk_rq_add_page(rq, pg, off, (uint32_t)len);
        taken += (uint32_t)len;
    }
    uint32_t extra = taken < left ? taken % disk->logical_block : 0;
    while (extra > 0) {   /* back off to a block boundary */
        BlkSeg *s = &rq->seg[rq->nsegs - 1];
        uint32_t cut = s->len < extra ? s->len : extra;
        s->len -= cut;
        rq->bytes -= cut;
        taken -= cut;
        extra -= cut;
        if (s->len == 0) rq->nsegs--;
    }
    return (int64_t)taken;
}

/* One logical block (len bytes) at va through a page of its own: for a block
 * that straddles pages on a one-segment device. Thread context. */
static int bounce_block(BlockDevice *dev, int op, uint64_t sector, uint8_t *va, uint32_t len) {
    struct page *pg = alloc_pages(0, 0);
    if (pg == NULL) return -ENOMEM;
    uint8_t *b = page_to_virt(pg);
    if (op == BLK_OP_WRITE) memcpy(b, va, len);
    int rc = blk_rw_sync(dev, op, sector, b, len);   /* page-aligned: one segment */
    if (rc == 0 && op == BLK_OP_READ) memcpy(va, b, len);
    free_pages(pg, 0);
    return rc;
}

int blk_rw_sync(BlockDevice *dev, int op, uint64_t sector, void *buf, size_t bytes) {
    if (op == BLK_OP_FLUSH)
        bytes = 0;   /* no data, one request */
    else if (bytes == 0)
        return -EINVAL;
    SyncWait w;
    sem_init(&w.done, 0);
    w.status = 0;
    BlkPlug plug;
    blk_plug_init(&plug);
    uint32_t nreq = 0;
    uint64_t pos = 0;
    int rc = 0;
    do {
        BlkRequest *rq = blk_rq_alloc(dev, op, sector + (pos >> BLK_SECTOR_SHIFT), sync_end_io, &w);
        if (rq == NULL) {
            rc = -ENOMEM;
            break;
        }
        if (op != BLK_OP_FLUSH) {
            int64_t n = fill_request(rq, (uintptr_t)buf + pos, bytes - pos);
            if (n <= 0) {
                blk_rq_free(rq);
                if (n < 0) {
                    rc = (int)n;
                    break;
                }
                uint32_t lbs = dev->disk->logical_block;
                uint32_t len = bytes - pos < lbs ? (uint32_t)(bytes - pos) : lbs;
                rc = bounce_block(dev, op, sector + (pos >> BLK_SECTOR_SHIFT), (uint8_t *)buf + pos, len);
                if (rc != 0) break;
                pos += len;
                continue;
            }
            pos += (uint64_t)n;
        }
        nreq++;
        blk_plug_add(&plug, rq);
    } while (pos < bytes);
    blk_plug_flush(&plug);

    BlockDevice *disk = dev->disk;
    for (uint32_t got = 0; got < nreq;) {
        if (sched_can_block()) {
            sem_wait(&w.done);
            got++;
        } else if (sem_trywait(&w.done) == 0) {
            got++;
        } else {   /* boot, before the scheduler: poll the device */
            blk_complete(disk);
            blk_expire(disk, hal_tsc_read());
            hal_cpu_relax();
        }
    }
    return rc != 0 ? rc : w.status;
}

/* --- Disks and partitions --- */

static int gpt_blk_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    BlockDevice *disk = ctx;
    uint32_t spb = disk->logical_block >> BLK_SECTOR_SHIFT;
    return blk_rw_sync(disk, BLK_OP_READ, lba * spb, buf, (size_t)count * disk->logical_block);
}

static void part_name(char *out, const char *disk, uint32_t n) {
    size_t len = strlen(disk);
    int digit = len > 0 && disk[len - 1] >= '0' && disk[len - 1] <= '9';
    ksnprintf(out, 16, "%s%s%u", disk, digit ? "p" : "", n);
}

static void add_partition(BlockDevice *disk, const GptPart *p) {
    BlockDevice *d = kmalloc(sizeof(BlockDevice), GFP_ZERO);
    if (d == NULL) return;
    uint32_t spb = disk->logical_block >> BLK_SECTOR_SHIFT;
    part_name(d->name, disk->name, p->number);
    d->disk = disk;
    d->start = p->first_lba * spb;
    d->nr_sectors = (p->last_lba - p->first_lba + 1) * spb;
    d->logical_block = disk->logical_block;
    d->physical_block = disk->physical_block;
    d->max_segments = disk->max_segments;
    d->max_transfer = disk->max_transfer;
    d->flags = disk->flags;
    d->has_gpt = 1;
    d->disk_guid = disk->disk_guid;
    d->part_guid = p->guid;
    d->type_guid = p->type;
    d->partno = p->number;
    memcpy(d->label, p->label, sizeof(d->label));
    if (registry_add(d) != 0) {
        kfree(d);
        return;
    }
    char type[GUID_STR_LEN], uuid[GUID_STR_LEN];
    gpt_guid_format(&p->type, type);
    gpt_guid_format(&p->guid, uuid);
    kprintf("[BLK] %s: start %lu, %lu sectors, type %s, PARTUUID %s, PARTLABEL \"%s\"\n", d->name, d->start,
            d->nr_sectors, type, uuid, d->label);
}

static void scan_gpt(BlockDevice *disk) {
    uint32_t lbs = disk->logical_block;
    void *scratch = kmalloc(2 * lbs, 0);
    GptPart *parts = kmalloc(GPT_SCAN_MAX * sizeof(GptPart), 0);
    GptDisk g;
    int rc = scratch && parts ? gpt_read(gpt_blk_read, disk, lbs, disk->nr_sectors / (lbs >> BLK_SECTOR_SHIFT), scratch,
                                         &g, parts, GPT_SCAN_MAX)
                              : -ENOMEM;
    if (rc == 0) {
        char guid[GUID_STR_LEN];
        disk->has_gpt = 1;
        disk->disk_guid = g.disk_guid;
        gpt_guid_format(&g.disk_guid, guid);
        kprintf("[BLK] %s: GPT %s, %u partitions%s\n", disk->name, guid, g.nparts,
                g.backup_used ? " (primary header bad: backup used)" : "");
        if (g.skipped) kprintf("[BLK] %s: %u GPT entries skipped (out of range or too many)\n", disk->name, g.skipped);
        for (uint32_t i = 0; i < g.nparts; i++) add_partition(disk, &parts[i]);
    } else if (rc != -ENOENT) {
        kprintf("[BLK] %s: no usable GPT (%d)\n", disk->name, rc);
    }
    kfree(parts);
    kfree(scratch);
}

static int queue_init(BlockDevice *disk, uint32_t depth) {
    BlkQueue *q = kmalloc(sizeof(BlkQueue), GFP_ZERO);
    if (q == NULL) return -ENOMEM;
    if (depth == 0) depth = 1;
    if (depth > BLK_MAX_DEPTH) depth = BLK_MAX_DEPTH;
    q->tags = kmalloc(depth * sizeof(BlkRequest *), GFP_ZERO);
    q->free_tags = kmalloc(depth * sizeof(uint16_t), 0);
    if (q->tags == NULL || q->free_tags == NULL) {
        kfree(q->tags);
        kfree(q->free_tags);
        kfree(q);
        return -ENOMEM;
    }
    spin_lock_init(&q->lock, LOCK_LEVEL_BLK_QUEUE, "block queue");
    q->depth = depth;
    for (uint32_t t = 0; t < depth; t++) q->free_tags[t] = (uint16_t)(depth - 1 - t);
    q->nfree_tags = depth;
    q->timeout_cycles = diag_ms_to_cycles(BLK_TIMEOUT_MS);
    disk->queue = q;
    return 0;
}

int blk_disk_register(BlockDevice *disk, const char *prefix, uint32_t depth) {
    if (rq_cache == NULL) {   /* first disk: thread context, during probe */
        if (kmem_cache_setup(&rq_cache_store, "blk_request", sizeof(BlkRequest), 8) != 0) return -ENOMEM;
        rq_cache = &rq_cache_store;
    }
    if (disk->logical_block != 512 && disk->logical_block != 4096) return -EINVAL;
    if (disk->max_segments == 0 || disk->max_transfer < PAGE_SIZE) return -EINVAL;   /* a page always fits */
    int rc = queue_init(disk, depth);
    if (rc != 0) return rc;
    disk->disk = disk;
    disk->start = 0;
    disk->partno = 0;
    if (disk->name[0] == '\0') {   /* else the driver named it ("nvme0n1") */
        unsigned same = 0;
        BlockDevice *d;
        size_t plen = strlen(prefix);
        for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
            if (d->partno == 0 && strncmp(d->name, prefix, plen) == 0) same++;
        }
        ksnprintf(disk->name, sizeof(disk->name), "%s%c", prefix, (char)('a' + same % 26));
    }
    rc = registry_add(disk);
    if (rc != 0) {   /* nothing was submitted, so no interrupt can reach the queue */
        BlkQueue *q = disk->queue;
        disk->queue = NULL;
        kfree(q->tags);
        kfree(q->free_tags);
        kfree(q);
        return rc;
    }
    kprintf("[BLK] %s: %lu sectors (%lu MiB), %u/%u-byte blocks, depth %u, %u segments, %u KiB per request%s%s\n",
            disk->name, disk->nr_sectors, disk->nr_sectors >> 11, disk->logical_block, disk->physical_block,
            disk->queue->depth, disk->max_segments, disk->max_transfer >> 10,
            (disk->flags & BLK_RDONLY) ? ", read-only" : "", (disk->flags & BLK_CACHE) ? ", write cache" : "");
    scan_gpt(disk);
    return 0;
}
