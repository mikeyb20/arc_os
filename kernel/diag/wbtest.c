/* arc_os — Write-back test (M1-14, test mode only)
 *
 * Owns: nothing past its run. Suite "writeback" boots with a disk whose GPT
 * partition "wbtest" is scratch space (the ctest fixture makes it zeroed).
 * In place of init, a kernel thread maps the partition through a
 * block-device page-cache mapping (the one ext2 keeps its metadata in) and
 * checks each way dirty pages reach the disk:
 *   - nothing waiting for write-back (ramfs pages, the boot modules among
 *     them, count as dirty but have nowhere to go): the flusher's timer is
 *     not armed and the flusher stays asleep;
 *   - pages dirtied: the timer is armed, the pages wait (no write in the
 *     first seconds), then the flusher writes them and the timer is gone;
 *   - a thread waiting for write-back (short of memory, or a writer over the
 *     dirty limit) starts the flusher at once, not after 5 s; with nothing
 *     to write its wait ends when its time runs out;
 *   - sync writes at once and flushes the disk's write cache; releasing the
 *     mapping (unmount) writes and flushes too.
 * With arc.wbtest=verify (the second boot, a new QEMU on the same disk
 * image) it reads the partition straight from the disk, bypassing the cache,
 * and checks every page the first boot wrote. Results are "[WBTEST] ..."
 * lines (with ARC_TEST PASS/FAIL wbtest-N), then ARC_TEST DONE; the thread
 * ends the run.
 * Invariants: waits are bounded by the TSC. Locks: none of its own.
 * Cost: test mode only. */

#include "diag/diag.h"
#include "drivers/blkdev.h"
#include "mm/filemap.h"
#include "mm/writeback.h"
#include "mm/kmalloc.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "arch/hal.h"
#include "lib/kprintf.h"
#include "lib/cmdline.h"
#include "lib/string.h"
#include "lib/testmode.h"

#define WB_FLUSH_PAGES 64    /* written by the flusher: pages 0-63 */
#define WB_SYNC_FIRST  64    /* by sync: 64-95 */
#define WB_SYNC_PAGES  32
#define WB_WAIT_FIRST  96    /* for a thread waiting for write-back: 96-111 (112-127 stay zero) */
#define WB_WAIT_PAGES  16
#define WB_REL_FIRST   128   /* when the mapping is released: 128-159 */
#define WB_REL_PAGES   32
#define WB_LAST_PAGE   160
#define WB_WAIT_MS     20000

static int passes, fails;

/* One check: a line for the reader, one for the harness (numbered in order) */
static void wb_result(int ok, const char *what) {
    kprintf("[WBTEST] %s %s\n", what, ok ? "ok" : "FAILED");
    kprintf("ARC_TEST %s wbtest-%d\n", ok ? "PASS" : "FAIL", passes + fails + 1);
    if (ok)
        passes++;
    else
        fails++;
}

/* Word w of page index i as written by round gen (1 to 4) */
static uint64_t wb_pattern(uint64_t i, uint64_t w, uint64_t gen) {
    return 0xA5C3000000000000ull | (gen << 40) | (i << 12) | w;
}

static uint64_t gen_of(uint64_t i) {
    if (i < WB_FLUSH_PAGES) return 1;
    if (i >= WB_SYNC_FIRST && i < WB_SYNC_FIRST + WB_SYNC_PAGES) return 2;
    if (i >= WB_REL_FIRST && i < WB_REL_FIRST + WB_REL_PAGES) return 3;
    if (i >= WB_WAIT_FIRST && i < WB_WAIT_FIRST + WB_WAIT_PAGES) return 4;
    return 0;   /* never written: zeros */
}

/* Fill page index i of the mapping with its pattern and mark it dirty */
static int dirty_page(AddressSpace *as, uint64_t i) {
    unsigned off;
    int err = 0;
    struct page *pg = filemap_get_page(as, i, NULL, 0, &off, &err);
    if (pg == NULL) return err;
    page_lock(pg);
    uint64_t *w = page_to_virt(pg);
    for (uint64_t k = 0; k < PAGE_SIZE / 8; k++) w[k] = wb_pattern(i, k, gen_of(i));
    filemap_set_dirty(pg);
    page_unlock(pg);
    page_put(pg);
    return 0;
}

static int dirty_range(AddressSpace *as, uint64_t first, uint64_t n) {
    for (uint64_t i = first; i < first + n; i++) {
        if (dirty_page(as, i) != 0) return -1;
    }
    return 0;
}

static uint64_t ms_since(uint64_t t0) {
    uint64_t per_ms = diag_ms_to_cycles(1);
    return per_ms != 0 ? (hal_tsc_read() - t0) / per_ms : 0;
}

static void run_write(BlockDevice *dev) {
    WbStats s0, s;
    FilemapStats fs;

    /* Nothing to write back: no timer, and the flusher sleeps */
    wb_stats(&s0);
    filemap_stats(&fs);
    uint64_t dirty0 = fs.dirty;   /* ramfs pages: never written back */
    wb_result(s0.listed == 0 && s0.writeback == 0 && !wb_timer_armed(),
              "clean: nothing listed for write-back, flusher timer not armed");
    diag_delay_ms(2000);
    wb_stats(&s);
    wb_result(s.flushes == s0.flushes && s.timer_arms == s0.timer_arms && !wb_timer_armed(),
              "idle 2 s: no flusher pass, no timer");

    static AddressSpace as;   /* lives past release: write-back may still name it until forget */
    filemap_bdev_init(&as, dev, (loff_t)dev->nr_sectors * 512);

    /* Dirtied: the timer arms, the pages wait for it */
    wb_result(dirty_range(&as, 0, WB_FLUSH_PAGES) == 0, "64 pages dirtied in the block-device cache");
    filemap_stats(&fs);
    wb_stats(&s);
    wb_result(fs.dirty == dirty0 + WB_FLUSH_PAGES && s.listed == 1 && wb_timer_armed() &&
                  s.timer_arms == s0.timer_arms + 1,
              "flusher timer armed once for the dirty mapping");
    uint64_t t0 = hal_tsc_read();
    diag_delay_ms(2000);
    filemap_stats(&fs);
    wb_stats(&s);
    wb_result(fs.dirty == dirty0 + WB_FLUSH_PAGES && s.flushes == s0.flushes,
              "still dirty after 2 s (the flusher waits 5 s)");

    /* The flusher writes them, and nothing is armed afterwards */
    for (;;) {
        filemap_stats(&fs);
        wb_stats(&s);
        if ((fs.dirty == dirty0 && s.written >= s0.written + WB_FLUSH_PAGES) || ms_since(t0) > WB_WAIT_MS) break;
        sched_yield();
    }
    kprintf("[WBTEST] flusher: %lu pages written %lu ms after they were dirtied\n", s.written - s0.written,
            ms_since(t0));
    diag_delay_ms(100);   /* the pass is counted after its last write */
    filemap_stats(&fs);
    wb_stats(&s);
    wb_result(fs.dirty == dirty0 && s.written == s0.written + WB_FLUSH_PAGES && s.flushes == s0.flushes + 1,
              "flusher wrote the 64 pages in one pass");
    wb_result(!wb_timer_armed() && s.listed == 0, "after the flush: flusher timer not armed");

    /* A thread waiting for write-back starts the flusher now */
    wb_stats(&s0);
    wb_result(dirty_range(&as, WB_WAIT_FIRST, WB_WAIT_PAGES) == 0 && wb_timer_armed(), "16 more dirtied, timer armed");
    t0 = hal_tsc_read();
    int moved = wb_wait_progress(WB_DELAY_MS / 2);
    uint64_t waited = ms_since(t0);
    kprintf("[WBTEST] waiter: write-back progress after %lu ms\n", waited);
    wb_result(moved && waited < WB_DELAY_MS / 2, "a waiter starts the flusher at once (before its 5 s timer)");
    for (;;) {
        wb_stats(&s);
        if (s.written >= s0.written + WB_WAIT_PAGES || ms_since(t0) > WB_WAIT_MS) break;
        sched_yield();
    }
    diag_delay_ms(100);   /* the pass ends after its last write */
    filemap_stats(&fs);
    wb_stats(&s);
    wb_result(fs.dirty == dirty0 && s.written == s0.written + WB_WAIT_PAGES && !wb_timer_armed() && s.listed == 0,
              "the flusher wrote the 16 pages, timer disarmed");
    t0 = hal_tsc_read();
    moved = wb_wait_progress(300);
    waited = ms_since(t0);
    kprintf("[WBTEST] waiter with nothing to write: woke after %lu ms\n", waited);
    wb_result(!moved && waited >= 250 && waited < 2000, "with nothing to write, a wait ends after its time (300 ms)");

    /* sync writes at once, flushes the disk's write cache, and leaves no timer */
    int cached = (dev->disk->flags & (BLK_CACHE | BLK_RDONLY)) == BLK_CACHE;
    kprintf("[WBTEST] disk %s: %s\n", dev->disk->name, cached ? "volatile write cache" : "no write cache");
    wb_stats(&s0);
    wb_result(dirty_range(&as, WB_SYNC_FIRST, WB_SYNC_PAGES) == 0 && wb_timer_armed(), "32 more dirtied, timer armed");
    wb_sync_all();
    filemap_stats(&fs);
    wb_stats(&s);
    wb_result(fs.dirty == dirty0 && s.written == s0.written + WB_SYNC_PAGES && !wb_timer_armed() && s.listed == 0 &&
                  (!cached || s.device_flushes > s0.device_flushes),
              "sync wrote 32 pages and flushed the disk cache, timer disarmed");

    /* Releasing the mapping (what unmount does) writes the rest */
    wb_stats(&s0);
    wb_result(dirty_range(&as, WB_REL_FIRST, WB_REL_PAGES) == 0, "32 more dirtied before release");
    filemap_bdev_release(&as);
    filemap_stats(&fs);
    wb_stats(&s);
    wb_result(fs.dirty == dirty0 && s.written == s0.written + WB_REL_PAGES && !wb_timer_armed() && s.listed == 0 &&
                  s.device_flushes == s0.device_flushes + 1,
              "release wrote 32 pages and flushed the disk cache, timer disarmed");
}

/* Second boot: the disk itself, not the cache */
static void run_verify(BlockDevice *dev) {
    uint64_t *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (buf == NULL) {
        wb_result(0, "verify: buffer");
        return;
    }
    uint64_t bad = 0;
    for (uint64_t i = 0; i < WB_LAST_PAGE; i++) {
        if (blk_rw_sync(dev, BLK_OP_READ, i * (PAGE_SIZE / 512), buf, PAGE_SIZE) != 0) {
            bad++;
            continue;
        }
        uint64_t gen = gen_of(i);
        for (uint64_t k = 0; k < PAGE_SIZE / 8; k++) {
            if (buf[k] != (gen != 0 ? wb_pattern(i, k, gen) : 0)) {
                bad++;
                break;
            }
        }
    }
    kfree(buf);
    kprintf("[WBTEST] verify: %lu pages read from the disk, %lu wrong\n", (uint64_t)WB_LAST_PAGE, bad);
    wb_result(bad == 0, "verify: the first boot's writes survived the restart");
}

static void wbtest_main(void *arg) {
    (void)arg;
    char mode[16] = "write";
    (void)cmdline_get(cmdline_raw(), "arc.wbtest", mode, sizeof(mode));
    BlockDevice *dev = blk_find_label("wbtest", NULL);
    if (dev == NULL || dev->nr_sectors < WB_LAST_PAGE * (PAGE_SIZE / 512)) {
        wb_result(0, "partition \"wbtest\" found");
    } else if (strcmp(mode, "verify") == 0) {
        run_verify(dev);
    } else {
        run_write(dev);
    }
    kprintf("ARC_TEST DONE pass=%d fail=%d\n", passes, fails);
    testmode_exit(fails == 0 ? 0 : 1);
}

int wbtest_start(void) {
    if (!testmode_active() || strcmp(testmode_suite(), "writeback") != 0) return 0;
    Thread *t = thread_create(wbtest_main, NULL);
    if (t == NULL) return 0;
    sched_add_thread(t);
    return 1;
}
