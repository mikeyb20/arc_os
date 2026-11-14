# M1-15 — ext2 write

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M1-13, M1-14 | M1-22 |

**Goal.** ext2 is read-write: create, extend, truncate, rename and delete files and directories, and `e2fsck`
finds the result clean (ADR-0008).

**Read first.**
- `docs/decisions/0008-disk-layout-and-filesystem.md`, Notes of M1-13 and M1-14, the ext2 code from M1-13

**Build.**
1. Block and inode allocation from the bitmaps, near the inode or a goal block; free counts in the group
   descriptors and superblock.
2. `writepage`/`writepages`: allocate blocks for holes at write time, update indirect blocks.
3. Truncate (frees blocks including indirect chains), unlink, rename (same or different directory, replacing a
   target), mkdir/rmdir (`.`, `..`, link counts), fast symlinks.
4. Directory entries: insert and remove with correct `rec_len`; when changing a `dir_index` directory, clear its
   index flag as Linux ext2 does.
5. `fsync` writes data, then metadata. Superblock state is marked dirty on mount and clean on unmount. `sync`.
   A data partition found dirty at mount is mounted with an on-screen warning until `e2fsck` exists (M5-01).
6. Timestamps: mtime/ctime always; no access-time updates on arc_os partitions (ADR-0018: flash keys wear).
7. Host test scenarios on images, each followed by `e2fsck -fn` (must exit 0) and a content check with
   `debugfs -R dump`: 10 000 files; write, overwrite, extend, truncate; rename storms; deep trees; a 5 GiB file with
   holes.
8. QEMU: write a 1 GiB patterned file, sync, boot again on the same image, verify; temp file + `fsync` + `rename`.

**Done when.**
- [x] All host scenarios are `e2fsck`-clean and the QEMU persistence test passes.

**Watch out.**
- No journal: write data, then the inode, then the bitmaps, and document the order in the code.
- Journaling (ext3) is later; don't start it.
- L block: a clean stopping point is after allocation + file writes + truncate pass their e2fsck scenarios.

**Notes.**
- From M1-14 (and its D3 review): `fsync` ends with `blk_rw_sync(dev, BLK_OP_FLUSH, ...)` on the volume's
  device and returns `as->wb_err` if a page was given up (`filemap_fsync` shows the pattern); `sync` already
  flushes every cached disk. `writepage` runs with the page locked and no inode lock, and must not wait for the
  inode lock: a writer over the dirty limit waits for write-back while holding it. See M1-14 Notes.
- From M1-13: clear `ind_cache` on any block-map change; never make an `ext2_ro_forced` volume writable;
  clear `EXT2_INDEX_FL` on a `dir_index` directory you change (no htree upkeep); `ext2_start_page` copies a
  file's EOF block from the metadata cache, so keep that copy coherent with writes (or drop it). See M1-13 Notes.

**Notes (M1-15 as built; handoff).**
- **Where it lives.** `fs/ext2.c` (mount read-write, superblock state, inodes, `ext2_shutdown`),
  `fs/ext2_alloc.c` (bitmaps, counts, the metadata write order), `fs/ext2_map.c` (block map, allocation,
  truncate, file pages, `fsync`), `fs/ext2_dir.c` (entries, create/mkdir/unlink/rmdir/rename/symlink),
  `fs/ext2_internal.h` (what they share). In mm: asynchronous page writes (`filemap.c`, `PG_WRITEBACK`), the
  block-list helper `mm/filemap_block.c` (`FilemapBlocks`, `filemap_blocks_read/_write`), two write-back lists
  and batched passes (`writeback.c`). `sysmount.c` mounts the data partition read-write only when it was found
  on the boot disk or named by `arc.data=`; `sysmount_shutdown` ends writing.
- **Two choices put to Michael as cards (open when the PR went up), both built on the recommended option:**
  (1) write-back is asynchronous now (`writepage` starts the write, `PG_WRITEBACK` clears in `end_io`; fsync,
  truncate, removal and the data-before-metadata barrier wait on it); (2) the shared block-mapping helper in mm
  (Linux's mpage), which FAT32 can move onto later. If either answer differs, the rework is in those files.
- **Write order (no journal).** A pass writes file data, flushes the disk cache, then metadata. Metadata goes
  in two rounds (`ext2_meta_writepages`): commit the counts; round one = everything but allocation state
  (inode tables, indirect and directory blocks), wait; flush; round two = superblock, group descriptors,
  bitmaps. New indirect, directory and slow-symlink blocks are written synchronously before anything points at
  them, so no pointer on the disk ever leads to garbage. A crash between the rounds leaves blocks in use that the
  bitmaps call free (allocating) or freed blocks still marked used (a leak); e2fsck fixes both, and until M5-01
  runs it at boot, a dirty volume mounted read-write could hand such a block out twice (hence the warning).
  Residual, documented in `ext2_alloc.c`: (a) `fsync` of one file writes all of the volume's metadata, so
  another file's new block pointers can reach the disk before that file's data (stale bytes after a crash;
  Linux ext2 has the same); (b) kswapd writing a single metadata page under memory pressure skips the round
  order; (c) a freed block reused before the inode that freed it is on the disk.
- **Superblock state.** A read-write mount writes `s_state` without VALID (and the mount count) and flushes
  before returning; `ext2_shutdown` writes back, flushes, then restores the state found at mount, alone and
  last. A volume found not clean is mounted read-write with a WARNING line (e2fsck at boot is M5-01) and stays
  marked not clean.
- **For M4-11 (power-off):** nothing but test mode calls `sysmount_shutdown` yet (there is no reboot or
  power-off path). Until M4-11 calls it before power-off, every real boot after the first finds the data
  partition "not cleanly unmounted" and warns. Test mode calls it from `proc_exit` of init, before the thread
  is marked dead (it sleeps).
- **Limits kept on purpose.** Writing needs 4 KiB blocks (1 and 2 KiB volumes stay read-only, logged): a block
  is one page, so a freed block never shares a cache page with its old role. Reserved blocks (`s_r_blocks`)
  are not held back for root. No access times are written (the VFS never sets atime, ADR-0018). A store
  through a shared mapping gets its block at write-back: on a full volume the page is dropped and
  `msync`/`fsync` say `-ENOSPC` (Linux behaves the same); mmap stores don't update mtime.
- **Locks.** `Ext2Vol.alloc_lock` at the new level 32 (`LOCK_LEVEL_FS_ALLOC`), under a file page and before a
  metadata page; at most one metadata page lock at a time, never held while taking `alloc_lock`;
  `ext2_meta_lock` waits for the page's write (stable metadata). `LOCKDEP_MAX_SLEEP_HELD` went 6 to 8 (deepest
  path: rename that grows a directory, 7). docs/kernel/locking.md has the rows.
- **Found while building:** a file opened before `ext2_shutdown` could still write between `SB_RDONLY` and the
  end of the write-back (`ext2_file_write` now checks `SB_RDONLY` too). `filemap_wait_range_if` reports
  `-ENOSPC` instead of `-EIO` when the mapping's kept error says no block could be found.
- **Not in the kernel yet (FYI):** no `symlink`/`symlinkat` syscall (only `vfs_symlink_at`, used by the host
  tests) and no `truncate(2)` (ENOSYS; `ftruncate` works); `PATH_MAX` is 512 (`fs/path.h`) where Linux has 4096,
  so symlink targets stop at 511 bytes.
- **Tests.** Host: `test_ext2w_runner` (its own program, one suite per process: volumes are never unmounted and
  the VFS has 16 mount slots; a 32 MiB page arena): suites files, dirs, space, state, 17 scenarios on images
  mkfs.ext2 makes in sparse temp files mapped shared; each ends with `ext2_shutdown` + `e2fsck -fn` (most also
  run e2fsck after a sync while still mounted) and content checks with `debugfs -R dump/stat`. A deliberate
  bitmap bug (blocks not freed) fails four of them. The host has no flusher thread: tests call `wb_sync_all`,
  and a wait runs deferred completions and moves the clock (`ew_sleep`). QEMU: `qemu.ext2w` (1 GiB file by temp
  + fsync + rename, an atomic replace, a tree; host e2fsck after) and `qemu.ext2w.verify` (second boot reads it
  all, frees the gigabyte; host e2fsck checks every block but the 103 the files hold is free). Under TCG here
  the 1 GiB write took 23.5 s and fsync 2.2 s.
- **From #45's D3 review (write-back timer), for the cleanup PRs:** T1 (stale flusher callback) and T2 (the
  throttle tests must see the wake timer fire) are in `tests/test_reclaim.c`; N1's comment is fixed. Still
  open: T3, `kernel/diag/wbtest.c` bounds the 5 s flusher delay only as 2-20 s (suggested: check about 4-10 s).

