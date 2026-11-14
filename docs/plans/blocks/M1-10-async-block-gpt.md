# M1-10 — Async block layer and GPT

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-09 | M1-12, M1-13, M4-04, M4-05, M4-06 |

**Goal.** Block I/O is asynchronous with many requests in flight and interrupt completion, disks are split into
GPT partitions, and every later storage driver plugs into the same queue (ADR-0005, ADR-0008).

**Read first.**
- `docs/decisions/0005-async-block-layer.md`, `0008-disk-layout-and-filesystem.md`, `0018-portable-usb-mode.md`
- `kernel/drivers/blkdev.c`, `blkdev.h`, `virtio_blk.c`, `kernel/fs/fat32.c` (how it reads blocks today)
- Notes of M1-09

**Build.**
1. Request (`bio`-like): device, 64-bit start sector, list of (page, offset, length) segments, operation
   (read / write / flush), completion callback and status.
2. Per-device queue with a depth limit, plugging (batch submit) and merging of adjacent requests;
   `blk_rw_sync()` for callers that must wait.
3. Device properties: logical block size (512 or 4096), physical block size, 64-bit capacity, maximum segments and
   transfer size.
4. virtio-blk: many requests in flight, interrupt completion, flush.
5. GPT: protective MBR, primary header and entries with CRC32 checks, backup header fallback. Partitions become
   block devices (`/dev/vda1` …) with PARTUUID, PARTLABEL and type GUID. Each disk exposes its GPT disk GUID.
   Lookup calls: by disk GUID + partition type GUID, and by label. The arc_os type GUIDs and labels (ADR-0018) live
   in one header that the host image tools use too.
6. FAT32 (read-only) moves onto the new API.
7. Tests: host tests on GPT images made with `sgdisk` (valid; corrupt primary → backup used; 4Kn); QEMU: 64
   concurrent reads complete; partitions appear; FAT32 tests pass.

**Done when.**
- [ ] Suites pass; no code outside drivers calls a driver's block functions directly.

**Cost.** Submit O(1) plus a merge check against the queue tail only; completion in IRQ allocates nothing (requests come from a slab cache at submit); segments carry pages, never copies.

**Watch out.**
- Completions run in interrupt context: no sleeping; wake the waiters instead.
- Requests carry pages, because the page cache (M1-12) hands pages straight to the block layer.

**Notes.**
- From M1-12 (built first, Michael's choice 2026-11-14): the page cache does every disk read and write through
  `filemap_bdev_io` (`kernel/mm/filemap.c`), which calls `dev->read` / `dev->write` synchronously, and
  `readpage` / `readpages` end each page with `filemap_end_read`. Swap that one call for a request whose
  completion calls `filemap_end_read`; page waiters already sleep on `PG_LOCKED`. FAT32's `readpages` gets
  batches of up to 32 consecutive pages: make each run of contiguous clusters one request. FAT32 metadata
  (`fat32_read_cluster`, mount) still calls `dev->read`. See M1-12's Notes.
