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
6. Timestamps: mtime/ctime always; atime only when older than mtime (relatime-like).
7. Host test scenarios on images, each followed by `e2fsck -fn` (must exit 0) and a content check with
   `debugfs -R dump`: 10 000 files; write, overwrite, extend, truncate; rename storms; deep trees; a 5 GiB file with
   holes.
8. QEMU: write a 1 GiB patterned file, sync, boot again on the same image, verify; temp file + `fsync` + `rename`.

**Done when.**
- [ ] All host scenarios are `e2fsck`-clean and the QEMU persistence test passes.

**Watch out.**
- No journal: write data, then the inode, then the bitmaps, and document the order in the code.
- Journaling (ext3) is later; don't start it.
- L block: a clean stopping point is after allocation + file writes + truncate pass their e2fsck scenarios.

**Notes.**
-
