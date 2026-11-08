# M1-12 — Page cache and file-backed mmap

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M1-07, M1-10, M1-11 | M1-13, M1-14 |

**Goal.** One page cache serves `read`/`write` and `mmap` for every regular file, so mapped and read data are
always the same bytes (ADR-0004, ADR-0016).

**Read first.**
- `docs/decisions/0004-page-cache-and-mmap.md`, `0016-kernel-memory-sizing.md`
- Notes of M1-02, M1-07, M1-10, M1-11; `kernel/mm/vmm.c`, `kernel/fs/vfs.c`, `kernel/fs/ramfs.c`
- `docs/plans/work-blocks.md` §7 (lock order)

**Build.**
1. `struct address_space` per inode: page index → page (radix tree), a lock, page count, ops (`readpage`,
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
8. Tests: write with `write()` and read through `mmap`, and the reverse (coherence); `MAP_PRIVATE` writes never
   reach the file; readahead counters move; a 1 GiB ramfs file mapped and summed by four processes.

**Done when.**
- [ ] Tests pass; FAT32 reads go through the cache.

**Watch out.**
- Lock order (§7): inode → mm → page → mapping-tree → page-table → page-cache index. Never take the mm lock while
  holding a page lock.
- A cache page can be mapped by many processes; keep `mapcount` right, reclaim depends on it.
- L block: a clean stopping point is after steps 1–3 with read/write tests passing.

**Notes.**
-
