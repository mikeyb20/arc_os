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
   block devices (`vda1` …, in the block layer's registry) with PARTUUID, PARTLABEL and type GUID. Each disk exposes its GPT disk GUID.
   Lookup calls: by disk GUID + partition type GUID, and by label. The arc_os type GUIDs and labels (ADR-0018) live
   in one header that the host image tools use too.
6. FAT32 (read-only) moves onto the new API.
7. Tests: host tests on GPT images made with `sgdisk` (valid; corrupt primary → backup used; 4Kn); QEMU: 64
   concurrent reads complete; partitions appear; FAT32 tests pass. (Changed in M1-10: `sgdisk` can't set the
   sector size of an image file, so the 4Kn image comes from `tools/make-gpt-image.py`, whose 512-byte output
   the image script checks with `sgdisk -v`. No `/dev/vda1` nodes yet: devfs is static and nothing opens a
   disk from user space before the installer; partitions are block-layer devices that the kernel looks up.)

**Done when.**
- [x] Suites pass; no code outside drivers calls a driver's block functions directly.

**Cost.** Submit O(1) plus a merge check against the queue tail only; completion in IRQ allocates nothing (requests come from a slab cache at submit); segments carry pages, never copies.

**Watch out.**
- Completions run in interrupt context: no sleeping; wake the waiters instead.
- Requests carry pages, because the page cache (M1-12) hands pages straight to the block layer.

**Notes.**
- 2026-11-14 (built). `drivers/blkdev.{h,c}`: requests (`BlkRequest`: disk, 512-byte sector, up to 16
  page segments, op, `end_io(priv, status)`), a queue per disk (depth up to 128 tags, pending FIFO, flight list
  in dispatch order), plugging (`BlkPlug`) with back-merging against the plug tail and the pending tail only,
  `blk_rw_sync` for kernel buffers (splits at pages, `max_segments`, `max_transfer` and logical-block
  boundaries), `blk_submit_page` for one page, partitions as devices with lookups by name, label and disk +
  type GUID. `drivers/gpt.{h,c}` is a pure parser over a read callback (host-tested on image files).
  `include/arc_disk.h` holds the ADR-0018 GUIDs and labels plus the ESP type. virtio-blk: any number of disks,
  one chain per request (header, segments mapped with `dma_map_page`, status), one doorbell per batch,
  negotiates `SEG_MAX`, `BLK_SIZE`, `TOPOLOGY`, `FLUSH`, `RO`.
- Contract (M1-12 swap): `end_io` runs exactly once per request, in interrupt context (or at once in the
  submitter when the request fails its checks), with no block-layer lock held; then the request is freed. For
  per-page completion submit one request per page on a plug: adjacent ones still reach the disk as one device
  request, each keeping its own `end_io`. Told the M1-12 thread (its review found one cache page can need
  several reads: a per-page "pieces pending" counter on its side fits this).
- Waiting: `blk_rw_sync` sleeps on a semaphore, or polls the device when the scheduler can't sleep yet
  (`sched_can_block()`, new: the boot thread before `sched_start`). So GPT scans and the boot FAT32 mount poll.
- Time-bounded completion (M1-09's ask): every dispatched request has a deadline (30 s). `blk_expire` polls the
  driver first (a lost interrupt: completes, counted as `polled`, logged once), and if the oldest is still
  there it resets the device (`ops->reset`, virtio: status 0, so it stops DMA before any buffer goes back) and
  fails everything with -EIO; the disk stays dead (no re-init: hot-unplug/re-probe is M4). It runs from the PIT
  tick through a new `hal_timer_set_tick_hook` until M1-06 replaces the tick (note added to M1-06). This also
  fixes M1-09's latent bug for virtio-blk (freeing DMA buffers the device may still write); virtio-net TX still
  has it (its owner is the network block).
- Locks: `BlkQueue.lock` level 85 (IRQ-safe) and the registry lock level 66; see docs/kernel/locking.md "Block
  layer locks".
- Tests: host `gpt` (8: sgdisk image, broken primary header, broken primary entries, 4Kn, no GPT, too many
  parts, both headers bad, GUID text) and `blkdev` (14, real blkdev.c over a fake driver and a page arena:
  64 in flight with one doorbell, depth limit, merging within limits, pending-tail merge, BLK_BUSY requeue,
  splitting of unaligned buffers, 4Kn cuts, refusals, flush without a cache, lost interrupt, hung device
  reset, names). QEMU `qemu.devices` and `.intx`: the disk is now GPT (ESP with the FAT32 volume + "blktest"
  pattern partition); the kernel's `diag/blktest.c` logs `[BLKTEST]` lines (64 reads in flight, 8 merged into
  1, sync read across pages, write + flush + read back, refusals, dropped interrupt completed by the timeout
  poll), and the suite reads `/disk/hello.txt` from a program, which sleeps until the disk interrupt (MSI-X,
  and INTx in `.intx`). `qemu.vfs` still mounts a whole-disk FAT32 image.
- Measured: kernel image 506844 bytes (limit 511000, unchanged). M1-12 measured 515584 on its own branch, so
  whichever of #42 and this block merges second raises the limit (measure, don't guess).
- Not done / for later: no `/dev` nodes for disks (spec changed above). FAT32 on a 4Kn disk refuses to mount
  (`fat32.c` assumes 512-byte sectors; ESPs on 4Kn disks use 4096-byte FAT sectors): fix when a 4Kn machine
  needs it. A virtio config-change interrupt (capacity change) is still ignored. The FAT is read with one
  `blk_rw_sync` call now (was one call per sector).
