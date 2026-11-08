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
- [ ] Tests pass.

**Cost.** Reclaim work is proportional to pages scanned, never to total RAM, and stops at the high watermark; reverse mapping walks only the VMAs that map the file; no timer while nothing is dirty.

**Watch out.**
- Never evict a locked page or one under I/O.
- Reclaim may unmap pages of a process running on another CPU; it must use the shootdown, not a local flush.

**Notes.**
-
