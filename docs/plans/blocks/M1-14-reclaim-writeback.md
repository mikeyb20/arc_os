# M1-14 — Memory reclaim and writeback

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-12, M1-13 | M1-15, M1-22 |

**Goal.** Clean page-cache pages are evicted under memory pressure (unmapped first when mapped), and dirty pages
reach the disk without periodic polling (ADR-0004, ADR-0016, ADR-0017).

**Read first.**
- `docs/decisions/0004-page-cache-and-mmap.md`, `0016-kernel-memory-sizing.md`, `0017-power-management.md`
- Notes of M1-02, M1-07, M1-12, M1-13

**Build.**
1. Active and inactive LRU lists of cache pages; for mapped pages, the referenced state comes from page-table
   accessed bits (test and clear while scanning).
2. Watermarks (min, low, high). Allocation below low wakes the reclaim thread; below min, the allocating thread
   reclaims directly.
3. Reverse mapping for file pages: walk the address space's VMA list and unmap the page from each mm (TLB
   shootdown from M1-07). Anonymous pages are never evicted (no swap).
4. Writeback: dirty pages are written on `fsync`/`msync`, when reclaim meets them, on `sync` and unmount, and by a
   deferred flusher whose timer is armed only while dirty pages exist (5 s; no timer when everything is clean).
5. Out of memory with nothing reclaimable: kill the largest process with a clear log line, never panic.
6. `/proc/meminfo`: Cached, Dirty, Writeback, Active, Inactive.
7. Tests (QEMU `-m 512M`): read a 2 GiB ext2 file and stay above the min watermark; map and sum a 2 GiB ext2 file;
   dirty pages through the raw block-device cache get written back and survive a QEMU restart; an idle system with
   no dirty pages has no flusher timer armed.

**Done when.**
- [x] Tests pass.

**Cost.** Reclaim work is proportional to pages scanned, never to total RAM, and stops at the high watermark; reverse mapping walks only the VMAs that map the file; no timer while nothing is dirty.

**Watch out.**
- Never evict a locked page or one under I/O.
- Reclaim may unmap pages of a process running on another CPU; it must use the shootdown, not a local flush.

**Notes.**
- From M1-13: ext2 metadata lives in block-device mappings (`AddressSpace.host == NULL`, page locks at
  `LOCK_LEVEL_META_PAGE`); reclaim treats them like file pages. `ind_cache` holds a disk block number, not a
  page, so dropping metadata pages never leaves it dangling.

**Notes (M1-14 as built; handoff).**
- **Where it lives.** `mm/reclaim.c` (LRU lists, watermarks, kswapd, direct reclaim, drop_caches),
  `mm/writeback.c` (write-back list, flusher, sync), hooks in `filemap.c` / `filemap_mmap.c` / `pmm.c`.
  `GFP_RECLAIM` (gfp.h) marks the allocations that may reclaim: page faults (anonymous, cache, copy on write)
  and page-cache pages from read/write/readahead. `GFP_KERNEL` never reclaims (unlike Linux), so any caller may
  hold anything. Direct reclaim also needs no spinlock held and a context that can sleep, and it never writes.
- **Policy** is Linux's `page_check_references` on two lists: new cache pages go inactive; a read marks a page
  `PG_REFERENCED`, a second read makes it active (`FileRa.prev` skips repeated small reads inside one page). A
  mapped page needs two scans: the first clears its accessed bits and keeps it (`PG_REFERENCED`), a second one
  with no access since frees it (accessed again, or by two mappings: active). The active list ages back when it
  outgrows the inactive one. Scans run at priority 12 to 0 (1/4096 of the lists, doubling), batches of 32.
- **Watermarks** (`reclaim_watermarks_for`): min = sqrt(16 x KiB of RAM) (Linux's min_free_kbytes, 128 KiB to
  256 MiB); low = min + gap, high = min + 2 x gap, gap = max(min/4, RAM/512, 4 MiB). 512 MiB: 717/1741/2765
  pages in QEMU; 4 GiB: 2048/4096/6144; 32 GiB: 5792/22176/38560. One global LRU: no zones, so a failed
  `GFP_DMA32` allocation reclaims pages anywhere.
- **kswapd** runs until free >= high; a run that frees nothing sleeps until a page joins a list or write-back
  cleans some (`lru_gen`), not on every allocation below low. It writes dirty pages it meets (with the page
  trylocked and no other lock: a `writepage` must not assume the inode lock, see M1-15 below).
- **Write-back** (`writeback.h`): a mapping with a `writepage` goes on the list when its first page is dirtied
  and the flusher's timer is armed then (5 s); the list empty means no timer. A pass takes every listed mapping
  once and writes all its cached dirty pages (`filemap_write_range`); a page dirtied during the pass relists its
  mapping. `sync(2)` (162) waits for a running pass and does one. Evicting a still-linked inode and releasing a
  block-device mapping (unmount) write back first. ramfs has no `writepage`: its pages count as dirty in
  /proc/meminfo but never wait for the flusher.
- **Interim timer (to swap).** M1-06 (#45) brings one-shot timers and removes the PIT tick, `blk_tick` with it.
  Until then the flusher's timer is a deadline in uptime ms (`deadline_ms`) that `wb_tick()` checks from the tick
  (`blk_tick` in kmain.c). **Whichever of M1-06 and M1-14 merges second** replaces it, about 15 lines in
  `writeback.c`: a `static Timer wb_timer` with `timer_init(&wb_timer, wb_timer_fn, NULL)` in `wb_init`;
  `timer_arm_locked` becomes `if (!timer_pending(&wb_timer)) { timer_arm(&wb_timer, clock_mono_ns() + WB_DELAY_MS
  * 1000000ull); count }`, `timer_cancel_locked` becomes `timer_cancel(&wb_timer)`, `wb_timer_armed` reads
  `timer_pending`; `wb_timer_fn` is today's `wb_tick` body without the deadline test (it runs with no lock held,
  so it may take `wb_lock`; `wb_lock` is the timer's one owner, as M1-06's rules ask). The throttling wait's
  deadline (`wake_at_ms`) becomes a second Timer the same way: armed in `wb_wait_progress` for the earliest
  waiter, its callback wakes `progress_wq`. Delete `wb_tick`, its declaration and its call. qemu.writeback checks the result (timer armed once, written about 5 s later, then
  disarmed; idle: no arms). The idle budget `idle_timer_irqs_5s` then covers write-back too.
- **Found and fixed:** a page lock taken on a block-device page and released after the page left its mapping
  (unmount's `drop_pages_from`, and now reclaim) ended the checker's record at the wrong level, so the next
  metadata page lock panicked debug builds ("same level, not in subkey order"). The holder's level is now kept in
  the page (`PG_LOCK_META`). qemu.writeback's release step covers it.
- **Measured** (qemu.reclaim, TCG, 512 MiB): reading 2 GiB, kswapd alone freed 400k pages (scan = steal), the
  lowest free count was 1700 pages (min 717), no direct reclaim. Mapping 2 GiB: 399k pages unmapped, kswapd
  scanned 1.55 x what it freed (each mapped page needs two looks) and the reader stalled into direct reclaim 476
  times (free went to min - 1 once). Whole suite 74 s.
- **Known costs, not built (follow-ups if they show):** (1) every unmapped page gets its own TLB shootdown;
  Linux batches them per reclaim batch (TTU_BATCH_FLUSH), which needs the frees deferred past one flush. (2) A
  write-back pass, like `fsync` since M1-12, walks all cached pages of a listed mapping to find the dirty ones; a
  radix-tree dirty tag (Linux's PAGECACHE_TAG_DIRTY) would make it proportional to dirty pages. M1-15 is the
  first heavy writer and the natural owner. (3) `read()` that can't get a page after reclaim returns `-ENOMEM`;
  only page faults go on to the OOM killer (fault.c).
- **For M1-15 (ext2 write):** give ext2 file mappings and the block-device mapping a `writepage`; the flusher,
  kswapd, sync, fsync and evict then write them with nothing else to add. `writepage` is called with the page
  locked (sometimes trylocked by reclaim) and no inode lock; block allocation needs its own lock. Unmount must
  call `filemap_bdev_release` (it writes back). Direct reclaim takes the mapping lock (40) inside whatever the
  allocating thread holds; the checker allows 6 sleeping locks per thread (`LOCKDEP_MAX_SLEEP_HELD`), and a fault
  inside a read on ext2 can already reach 6 by the review's count (file position, inode, mm, page, metadata page,
  mapping lock), so a write path that adds one must count. ETXTBSY (writing a running program's file) still has
  no owner. Also from the D3 review (below): ext2's `fsync` must end with `BLK_OP_FLUSH` on its device and
  report the mapping's kept error; its `writepage` must not wait for the inode lock (a writer over the dirty
  limit holds it while it waits for write-back).
- **D3 review fixes (#47).**
  - *Private copies survive reclaim (was HIGH).* `filemap_unmap_page` zapped the entry at the page's index in
    every VMA, and in a written `MAP_PRIVATE` VMA that entry is the process's own copy. Now it takes an entry out
    only if it maps the page (`mm_zap_page_if`: checked and removed under one `pt_lock` hold, so `cow_install`
    swapping the entry cannot slip between). Truncate still takes private copies past the end (`filemap_unmap_from`,
    as Linux). Host `private_copies_survive_reclaim` and `zap_page_if_checks_the_frame`; qemu.reclaim writes a
    private copy of the big file's first page and checks it after 2 GiB of pressure.
  - *Dirty memory no longer means OOM.* Direct reclaim that frees nothing but met dirty pages waits for
    write-back (`wb_wait_progress`: the flusher starts at once; the wait ends when a page write or a pass
    finishes, or after `WB_THROTTLE_MS` = 100 ms) and retries; after 3 waits without progress it gives up as
    before. Writers (`filemap_write`) above `WB_DIRTY_BACKGROUND_PCT` (10%) of RAM in dirty pages for disks
    start the flusher; above `WB_DIRTY_LIMIT_PCT` (20%) they wait the same way (Linux's dirty ratios, of total RAM
    rather than Linux's "available"). `FilemapStats.dirty_wb` counts those pages (ramfs excluded). Not
    throttled: stores through shared mappings (only direct reclaim waits for them). kswapd never reclaims
    directly (a `writepage` reading metadata would recurse; Linux's `PF_MEMALLOC`). The wait's time limit is a
    second deadline (`wake_at_ms`) the interim tick checks: the M1-06 swap turns it into a second `Timer` too.
  - *Durability.* `sync(2)` flushes every writable disk with a volatile write cache (`BLK_CACHE`, `BLK_OP_FLUSH`)
    after its pass, as Linux's sync does (a flush changes no data, so foreign disks are safe under ADR-0018);
    releasing a block-device mapping (unmount) flushes its disk; `filemap_fsync` flushes a block-device
    mapping's disk. File systems flush their own device in `fsync` (M1-15).
  - *Failing writes stop.* A failed page stays dirty and is retried; once a mapping's writes have failed for
    `WB_GIVE_UP_MS` (30 s) with none getting through, failed pages are dropped clean, one `[WB]` log line says so,
    the error is kept in `as->wb_err` for the next `fsync`, and the flusher stops waking for them. A write that
    works resets the clock. Linux drops a failed page at once; 30 s rides out a short glitch.
  - *kswapd's gate.* A page losing its last mapping bumps `lru_gen` while kswapd sleeps after a failed run
    (`reclaim_note_freeable`), so it tries again.
  - /proc/vmstat: `wb_dirty_for_disks`, `wb_throttled`, `wb_dropped`, `wb_device_flushes`. qemu.writeback adds a
    waiter that starts the flusher before its timer, a wait that ends on time, and the flushes (15 checks).

