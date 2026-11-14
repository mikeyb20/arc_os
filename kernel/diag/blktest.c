/* arc_os — Block-layer test (M1-10, test mode only)
 *
 * Owns: nothing past its run. Suite "devices" boots with a GPT disk whose
 * partition "blktest" holds a pattern: every 64-bit word of 512-byte sector s
 * (counted from the partition start) is s. The test reads it through the
 * block layer: 64 requests in flight at once, adjacent reads merged, a sync
 * read into an unaligned buffer, a write + flush + read back, requests the
 * layer must refuse, and one request whose interrupt is dropped so that the
 * timeout path's poll completes it. Results are "[BLKTEST] ..." lines.
 * Invariants: runs on the BSP during boot, before the scheduler; async
 * requests complete by interrupt (the wait loop never polls the device),
 * sync ones poll (blk_rw_sync before the scheduler). Waits are TSC-bounded.
 * Locks: none of its own. Cost: test mode only. */

#include "diag/diag.h"
#include "drivers/blkdev.h"
#include "arch/hal.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "uapi/errno.h"
#include "arc_disk.h"

#define BT_READS        64
#define BT_ORDER        6   /* 64 pages */
#define BT_WAIT_MS      5000
#define BT_WRITE_SECTOR 6144

static volatile uint32_t bt_done;
static volatile int bt_err;

static void bt_end_io(void *priv, int status) {
    (void)priv;
    if (status != 0) bt_err = status;
    __atomic_add_fetch(&bt_done, 1, __ATOMIC_RELEASE);
}

/* Wait with interrupts on until want requests ended, at most ms. */
static int bt_wait(uint32_t want, uint32_t ms) {
    uint64_t limit = diag_ms_to_cycles(ms);
    uint64_t flags = irq_save();
    hal_enable_interrupts();
    uint64_t start = hal_tsc_read();
    while (__atomic_load_n(&bt_done, __ATOMIC_ACQUIRE) < want && hal_tsc_read() - start < limit) hal_cpu_relax();
    irq_restore(flags);
    return __atomic_load_n(&bt_done, __ATOMIC_ACQUIRE) >= want && bt_err == 0;
}

static int pattern_ok(const void *buf, uint64_t sector, uint32_t bytes) {
    const uint64_t *w = buf;
    for (uint32_t i = 0; i < bytes / 8; i++) {
        if (w[i] != sector + i / (BLK_SECTOR_SIZE / 8)) return 0;
    }
    return 1;
}

static struct page *nth(struct page *blk, unsigned i) {
    return pfn_to_page(page_to_pfn(blk) + i);
}

/* Queue one 4 KiB read of page pg at sector on the plug */
static int plug_read(BlkPlug *plug, BlockDevice *part, uint64_t sector, struct page *pg) {
    BlkRequest *rq = blk_rq_alloc(part, BLK_OP_READ, sector, bt_end_io, NULL);
    if (rq == NULL) return -ENOMEM;
    blk_rq_add_page(rq, pg, 0, PAGE_SIZE);
    blk_plug_add(plug, rq);
    return 0;
}

/* 64 reads 8 KiB apart (so none merge) in flight at once, ended by interrupts */
static void test_concurrent(BlockDevice *part, struct page *blk) {
    BlkStats st;
    BlkPlug plug;
    bt_done = 0;
    bt_err = 0;
    blk_plug_init(&plug);
    for (unsigned i = 0; i < BT_READS; i++) plug_read(&plug, part, (uint64_t)i * 16, nth(blk, i));
    blk_plug_flush(&plug);
    int ok = bt_wait(BT_READS, BT_WAIT_MS);
    for (unsigned i = 0; ok && i < BT_READS; i++)
        ok = pattern_ok(page_to_virt(nth(blk, i)), (uint64_t)i * 16, PAGE_SIZE);
    blk_get_stats(part, &st);
    kprintf("[BLKTEST] %u concurrent reads %s (max in flight %u)\n", BT_READS, ok ? "ok" : "FAILED", st.max_in_flight);
}

/* 8 adjacent 4 KiB reads on one plug become one device request */
static void test_merge(BlockDevice *part, struct page *blk) {
    BlkStats before, after;
    BlkPlug plug;
    blk_get_stats(part, &before);
    bt_done = 0;
    bt_err = 0;
    blk_plug_init(&plug);
    for (unsigned i = 0; i < 8; i++) plug_read(&plug, part, 2048 + (uint64_t)i * 8, nth(blk, i));
    blk_plug_flush(&plug);
    int ok = bt_wait(8, BT_WAIT_MS);
    for (unsigned i = 0; ok && i < 8; i++)
        ok = pattern_ok(page_to_virt(nth(blk, i)), 2048 + (uint64_t)i * 8, PAGE_SIZE);
    blk_get_stats(part, &after);
    uint64_t merged = after.merged - before.merged, started = after.dispatched - before.dispatched;
    kprintf("[BLKTEST] 8 adjacent reads merged into %lu request(s) %s\n", started,
            ok && merged == 7 && started == 1 ? "ok" : "FAILED");
}

/* Sync read into a buffer that starts mid-page, then write + flush + read back */
static void test_sync(BlockDevice *part) {
    uint8_t *buf = kmalloc(4 * PAGE_SIZE, 0);
    if (buf == NULL) return;
    int rc = blk_rw_sync(part, BLK_OP_READ, 100, buf + 512, 2 * PAGE_SIZE);
    kprintf("[BLKTEST] sync read across pages %s\n",
            rc == 0 && pattern_ok(buf + 512, 100, 2 * PAGE_SIZE) ? "ok" : "FAILED");

    memset(buf, 0x5A, PAGE_SIZE);
    int w = blk_rw_sync(part, BLK_OP_WRITE, BT_WRITE_SECTOR, buf, PAGE_SIZE);
    int f = blk_rw_sync(part, BLK_OP_FLUSH, 0, NULL, 0);
    memset(buf + PAGE_SIZE, 0, PAGE_SIZE);
    int r = blk_rw_sync(part, BLK_OP_READ, BT_WRITE_SECTOR, buf + PAGE_SIZE, PAGE_SIZE);
    int same = memcmp(buf, buf + PAGE_SIZE, PAGE_SIZE) == 0;
    kprintf("[BLKTEST] write, flush, read back %s\n", w == 0 && f == 0 && r == 0 && same ? "ok" : "FAILED");

    int past = blk_rw_sync(part, BLK_OP_READ, part->nr_sectors - 4, buf, PAGE_SIZE);
    int odd = blk_rw_sync(part, BLK_OP_READ, 0, buf, 100);
    kprintf("[BLKTEST] out-of-range and partial-sector requests refused %s\n",
            past == -EINVAL && odd == -EINVAL ? "ok" : "FAILED");
    kfree(buf);
}

/* The device finishes but its interrupt is dropped: the timeout path polls */
static void test_lost_irq(BlockDevice *part, struct page *blk) {
    BlkStats before, after;
    /* Interrupts the sync tests left pending (they poll, with this CPU's
     * interrupts off) arrive first, so the one dropped is this request's */
    bt_wait(UINT32_MAX, 20);
    blk_get_stats(part, &before);
    blk_test_set(part, 100, 1);
    bt_done = 0;
    bt_err = 0;
    BlkPlug plug;
    blk_plug_init(&plug);
    plug_read(&plug, part, 512, nth(blk, 0));
    blk_plug_flush(&plug);
    int early = bt_wait(1, 300);   /* the dropped interrupt must not end it */
    blk_expire(part->disk, hal_tsc_read());
    int ok = !early && bt_wait(1, 10) && pattern_ok(page_to_virt(nth(blk, 0)), 512, PAGE_SIZE);
    blk_get_stats(part, &after);
    blk_test_set(part, BLK_TIMEOUT_MS, 0);
    kprintf("[BLKTEST] lost interrupt: completed by the timeout poll %s\n",
            ok && after.polled > before.polled && after.timeouts == before.timeouts ? "ok" : "FAILED");
}

void blktest_run(void) {
    Guid esp_type;
    gpt_guid_parse(ARC_PART_TYPE_ESP, &esp_type);
    BlockDevice *part = blk_find_label("blktest", NULL);
    BlockDevice *esp = part != NULL ? blk_find_part(&part->disk_guid, &esp_type) : NULL;
    if (part == NULL || esp == NULL) {
        kprintf("[BLKTEST] partitions FAILED: no \"blktest\" partition beside an ESP\n");
        return;
    }
    kprintf("[BLKTEST] partitions: %s ESP, %s \"blktest\" on %s ok\n", esp->name, part->name, part->disk->name);
    struct page *blk = alloc_pages(BT_ORDER, 0);
    if (blk == NULL) return;
    test_concurrent(part, blk);
    test_merge(part, blk);
    test_sync(part);
    test_lost_irq(part, blk);
    free_pages(blk, BT_ORDER);

    BlkStats st;
    blk_get_stats(part, &st);
    kprintf("[BLKTEST] %s: submitted %lu merged %lu dispatched %lu completed %lu errors %lu polled %lu timeouts %lu\n",
            part->disk->name, st.submitted, st.merged, st.dispatched, st.completed, st.errors, st.polled, st.timeouts);
}
