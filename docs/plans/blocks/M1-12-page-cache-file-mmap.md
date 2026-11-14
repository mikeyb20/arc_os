# M1-12 — Page cache and file-backed mmap

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M1-07, M1-10, M1-11 | M1-13, M1-14 |

**Goal.** One page cache serves `read`/`write` and `mmap` for every regular file, so mapped and read data are
always the same bytes (ADR-0004, ADR-0016).

**Read first.**
- `docs/decisions/0004-page-cache-and-mmap.md`, `0016-kernel-memory-sizing.md`
- Notes of M1-02, M1-07, M1-10, M1-11; `kernel/mm/vmm.c`, `kernel/fs/vfs.c`, `kernel/fs/ramfs.c`
- `kernel/proc/proc_syscalls.c` (`exec_read_elf`, `sys_execve`), `kernel/proc/elf.c`
- `docs/plans/work-blocks.md` §7 (lock order)

**Build.**
1. `struct address_space` per inode: page index → page (radix tree; lookups return a page plus the offset inside
   it and never assume order 0, so 2 MiB file pages can come later without a rewrite; build order 0 only,
   ADR-0019), a lock, page count, ops (`readpage`,
   `readpages`, `writepage`, dirty marking), and the list of VMAs that map it (used by reclaim in M1-14).
2. Generic file read/write through the cache: find or create the page, fill it through the ops (async block I/O
   for disk filesystems), copy to or from user space following §7 (prefault, copy with faults disabled, retry on a
   short copy), mark dirty, update the size.
3. ramfs: its pages are the storage (never evicted while the file exists).
4. Block-device mapping (a cache for the raw device), used for ext2 metadata in M1-13.
5. File `mmap`: `MAP_SHARED` (writes land in cache pages, marked dirty) and `MAP_PRIVATE` (copy-on-write from cache
   pages); faults go through the VMA's file; `msync`, `fsync`, `fdatasync`.
6. Readahead on sequential reads and faults: start at 128 KiB, grow to 2 MiB; `madvise` `SEQUENTIAL`, `RANDOM` and
   `WILLNEED` adjust it.
7. Truncate drops cache pages past the new size and unmaps them.
8. exec through the cache (ADR-0004: no eager loading of executables). Today `exec_read_elf` copies the whole file
   into one kmalloc buffer, and kmalloc stops at 4 MiB, so arc-llm (7.7 MB) and the llama.cpp tools (11.8 MB) fail
   with `-ENOMEM`. Instead, `execve` reads only the ELF header and program headers, then maps each `PT_LOAD` segment
   as a `MAP_PRIVATE` file VMA (its bss tail and the rest of the last page zero-filled, the remainder anonymous);
   pages arrive by fault. Drop `MAX_ELF_SIZE`; the only limit is the address space. setuid/setgid checks stay.
9. Tests: write with `write()` and read through `mmap`, and the reverse (coherence); `MAP_PRIVATE` writes never
   reach the file; readahead counters move; a 1 GiB ramfs file mapped and summed by four processes; a static
   program over 8 MiB (padded with a large initialized array it checksums) execs, runs and exits with no page or
   heap leak; every existing exec test still passes.

**Done when.**
- [ ] Tests pass; FAT32 reads go through the cache.
- [ ] exec never copies a whole file into kernel memory; a program over 8 MiB starts (arc-llm needs this in M2-04b).

**Cost.** Page lookup at most 4 radix levels (64 slots each covers 64 GiB files); a mapped-file fault copies nothing; readahead issues I/O in batches of at least 128 KiB; tree nodes from a slab cache.

**Watch out.**
- Lock order (§7): inode → mm → page → mapping-tree → page-table → page-cache index. Never take the mm lock while
  holding a page lock.
- A cache page can be mapped by many processes; keep `mapcount` right, reclaim depends on it.
- L block: a clean stopping point is after steps 1–3 with read/write tests passing. Step 8 (exec) comes after file
  `mmap` (step 5) works; if the block passes 1.5× its size, step 8 is the part to split off, never to drop.

**Notes.**
- 2026-11-14 (built, before M1-10: Michael chose "start now, sync disk reads" on 2026-11-14). Page-cache disk
  I/O goes through two calls in `kernel/mm/filemap.c`. Reads come in pieces: a `readpage` calls
  `filemap_io_begin(pg)`, then `filemap_bdev_read` once per contiguous run of sectors (a FAT32 page can take
  several: clusters smaller than a page, a fragmented chain), then `filemap_io_end(pg, err)`. The page counts the
  pieces in flight in `flags` bits 16-23 (`PG_IO_MASK`, no allocation), and whichever finishes last, a piece or the
  end call, ends the read with `filemap_end_read` (`PG_ERROR` if any piece failed). Writes use
  `filemap_bdev_write`, which waits: `fsync`/`msync(MS_SYNC)` rely on that. **For M1-10:** make
  `filemap_bdev_read` submit a request (`blk_submit_page`, priv = the page) whose `end_io` calls
  `filemap_bdev_read_done(pg, status ? -EIO : 0)`, and return 0 once it is submitted. `filemap_end_read` is safe
  in an interrupt (atomic flag bits, IRQ-safe wait-queue lock, no allocation). Put a `readpages` batch on one
  plug so adjacent pieces reach the disk as one request. Make `filemap_bdev_write` submit and wait. FAT32
  metadata (boot sector, FAT, directory clusters: `fat32_read_cluster`, mount) still calls `dev->read` directly
  and moves to `blk_rw_sync`.
- Layout. `kernel/mm/radix.c` (index, own slab cache, shrinks back), `filemap.c` (address spaces, page lock with
  hashed wait queues, page references, LRU of evictable pages, read/write through the cache, readahead,
  write-back, truncate, block-device mapping, counters), `filemap_mmap.c` (file VMA ops: fault, copy on write,
  the mapping's VMA list, unmap on truncate, write-protect on write-back). ramfs pages are the storage
  (`AS_MEMBACKED`: never on the LRU, a missing page is a hole of zeros); FAT32 files are cached and evictable.
  `filemap_bdev_init` exists for M1-13's ext2 metadata; only host tests use it so far.
- Page references: the tree holds one, every PTE one more plus `mapcount`; `PG_FILE` stays set until the page is
  freed. Fork skips page-cache PTEs (`vmm_copy_range_in`'s skip callback): the child faults them back in, so
  `mapcount` is right without copying. A private store copies the page into an anonymous one (`cow_install`).
- Faults: a file page past EOF gives `MM_FAULT_SIGBUS` (SIGBUS, `BUS_ADRERR`). Mapped cache pages start
  read-only; a shared store marks the page dirty and makes the entry writable under the page lock, and write-back
  write-protects every mapping first (`filemap_mkclean`), so a later store dirties it again.
- exec: `exec_load` reads the ELF header and program headers (at most 64 KiB) through an `ElfSource`; whole
  file pages of each `PT_LOAD` become a private file VMA (`mm_setup_object_vma`); the page shared with the
  previous segment and the last partial page are copied, bss is anonymous. A segment with `(p_offset - p_vaddr)
  % 4096 != 0` falls back to copying. `MAX_ELF_SIZE` is gone. init uses `exec_load` too.
- Syscalls: `msync` (26) and `fdatasync` (75) added; `mmap` of files (EACCES/ENODEV/EBADF as Linux), `madvise`
  NORMAL/SEQUENTIAL/RANDOM/WILLNEED. `/proc/vmstat` is new (nr_file_pages, nr_dirty, nr_mapped, pgpgin in pages,
  ra_pages, ra_batches, pages_dropped); `/proc/meminfo` has Cached, Dirty, Mapped; `drop_caches` 1 drops clean
  unmapped cache pages and init's leak check writes 3.
- `Vma` grew from 96 to 120 bytes (file, pgoff, ops, mapping-list link); the size assert in `vma.h` says why.
- **For M1-14:** `filemap_drop_clean` walks the whole LRU under the LRU lock (fine for `drop_caches`, not for
  reclaim: needs a bounded scan with the lock dropped between batches). Mapped pages are skipped, never
  unmapped. ramfs and FAT32 have no `writepage` that reaches a disk (ramfs's only cleans), so nothing tests real
  write-back yet; dirty marking is generic (no filesystem op), which ext2 write-back may want later.
- Measured (QEMU TCG, 4 CPUs, 4 GiB): `qemu.pagecache` runs in about 13 s; four processes mapping a 1 GiB ramfs
  file show Mapped 1048632 kB and MemFree down 8592 kB (their page tables). Kernel image 486931 -> 515584 bytes.
- D3 review (#42) fixes: a shared-store fault takes its page reference under the page-table lock before
  `page_lock` (a truncate could free the page first); `vma_split` lists the upper half with its file before the
  lower half shrinks; a readahead or fault that inserts a page after a truncate's walk passed it takes it out
  again (`read_window` rechecks the end after the insert); `MAP_POPULATE` read-faults shared mappings (as
  Linux); a whole-page write past the end that copies nothing leaves no page; the page-wait buckets are set up
  in `filemap_init`. Host regression tests run a truncate inside the fault (`-Wl,--wrap=page_lock`, an
  `alloc_pages` hook).
- Open, not in this block's scope: no `ETXTBSY` (writing to a running program's file changes its mapped text);
  `MAP_32BIT` is ignored for file mappings; boot modules are copied into ramfs and their module memory is kept
  (pre-existing, about twice the module size).
