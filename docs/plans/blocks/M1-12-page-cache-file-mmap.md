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
-
