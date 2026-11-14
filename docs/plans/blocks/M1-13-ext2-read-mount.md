# M1-13 — ext2 read and mount

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K | M1-10, M1-11, M1-12 | M1-14, M1-15 |

**Goal.** arc_os mounts its own ext2 rev 1 partitions from the disk it booted from and reads files of any size
through the page cache (ADR-0008, ADR-0009, ADR-0018).

**Read first.**
- `docs/decisions/0008-disk-layout-and-filesystem.md`, `0009-directory-layout.md`, `0018-portable-usb-mode.md`
- `kernel/boot/bootinfo.h`, `kernel/boot/limine.c`
- The archived ext2 layout spec (M0-03), Notes of M1-10, M1-11, M1-12, `kernel/fs/fat32.c` as a template

**Build.**
1. Superblock and group descriptors. Supported: rev 1; 1, 2 and 4 KiB blocks; 128- and 256-byte inodes;
   `filetype`, `sparse_super`, `large_file`. Safe to ignore: `dir_index`, `ext_attr`, `resize_inode`. Any other
   incompat feature refuses the mount; any other ro_compat feature forces read-only. Log which.
2. Inode read; block map through direct, indirect, double and triple indirect blocks, with a small cache of the last
   indirect block per inode. Metadata goes through the block-device cache (M1-12).
3. Directories: linear read (htree directories read linearly), lookup, `getdents64`.
4. `readpage`/`readpages` for regular files through the block map and async requests; holes read as zero pages;
   fast and slow symlinks.
5. Mount at boot (ADR-0018): `BootInfo` gains the GPT disk GUID and partition GUID that Limine reports for the
   kernel file. The kernel mounts the arc_os system partition (read-only) and data partition from that disk by
   type GUID, falls back to the labels `arc-system`/`arc-data`, and accepts `arc.data=PARTUUID=<guid>` to name
   another arc_os disk. The data partition's `apps`, `models`, `packs`, `home` and `var` directories are mounted at
   `/apps`, `/models`, `/packs`, `/home` and `/var` with subtree mounts (M1-11). Partitions with any other type GUID
   are never mounted.
6. Host tests: the ext2 code built into the host test runner against images made with
   `mkfs.ext2 -b 4096 -I 256 -O large_file -d <dir>` (no root needed): nested directories, an empty file, a 5 GiB
   sparse file with data at the end, triple-indirect offsets, symlinks, a `dir_index` directory.
7. QEMU: a GPT disk with arc_os system and data partitions is mounted; a checksum program reads the files and
   matches the host. A second disk with foreign partitions (Linux and Windows type GUIDs) is left unmounted and its
   bytes are unchanged after the run.

**Done when.**
- [x] Host and QEMU tests pass.

**Watch out.**
- For regular files, `i_size_high` (old name `i_dir_acl`) holds the top 32 bits of the size.
- ext2 block numbers are 32-bit; that is fine for these disk sizes.
- Writing is M1-15. Mount read-only here.

**Notes.**
- From M1-11: symlinks need the path walk to follow links and handle ".." itself; today `path_normalize`
  resolves "..", trailing slashes and `x/..` on text before the walk. The `*at` calls and the cwd are path
  strings (a dirfd whose directory moved gives -ENOENT); a walk that starts from an inode fixes both, and
  rename's ancestry check (path text, exact only under `rename_mutex`) then has to walk ".." instead. See the
  M1-11 Notes.
- From M1-10: partitions are block devices named like Linux (`vda1`, `nvme0n1p1`), each with `disk_guid`,
  `part_guid` (PARTUUID), `type_guid` and `label`; find them with `blk_find_part(disk_guid, type)` and
  `blk_find_label(label)`, and parse GUID text with `gpt_guid_parse`. The arc_os type GUIDs and labels are in
  `kernel/include/arc_disk.h`; `tools/make-gpt-test-disk.sh` reads them from there with `sed`, so build the
  system/data test disk in that script. Any number of virtio-blk disks bind (`vda`, `vdb`, ...), so the
  foreign-partition second disk needs only another `-drive`/`-device` pair. Metadata reads: `blk_rw_sync` (or
  the M1-12 block-device mapping); file data: requests from `readpages`. `kmain.c`'s `disk_mount` mounts the
  first disk's ESP (or a whole-disk FAT32) at `/disk`; boot-disk mounting goes beside it.
- 2026-11-14 (built, PR #44; started on #42 before #43 merged, Michael's card; boot mount added after #43). **Path walk**
  (`fs/vfs.c`): from "/" or a start `PathLoc` (cwd, dirfd), one component at a time; `..` asks the filesystem's
  own `..` lookup (every filesystem answers it now: ramfs/fat32 keep a parent pointer, devfs/procfs return their
  root) and steps back over mount points through `Mount.parent`; symlinks are followed up to 40 per walk (2 x
  PATH_MAX buffer kmalloc'd only when a link is met); a trailing `/` wants a directory. `path_normalize` and
  `fs/path.c` are gone. The cwd is an inode reference (`Process.cwd` + `cwd_mnt`); getcwd walks `..` up and scans
  each parent for the child (`vfs_path_of`, O(depth x entries)); a dirfd keeps working after a rename
  (`File.mnt` replaced `File.path`). Rename finds ancestry by walking `..` under `rename_mutex`; the moved
  directory locks at 12, a replaced one at 13 (new `LOCK_LEVEL_INODE_CHILD_2`). New syscalls readlink (89) and
  readlinkat (267); lstat/newfstatat honour `AT_SYMLINK_NOFOLLOW`, open honours `O_NOFOLLOW`. Dot names give
  Linux's errnos (mkdir "." EEXIST, rmdir "." EINVAL and ".." ENOTEMPTY, unlink ".." EISDIR, rename "." EBUSY).
- **ext2** (`fs/ext2.c`, read-only): rev 1, 1/2/4 KiB blocks, 128/256-byte inodes; `filetype`, `sparse_super`,
  `large_file` used; `dir_index`, `ext_attr`, `resize_inode` ignored (htree directories read linearly); any other
  incompat feature refuses the mount, any other ro_compat one sets `ro_forced` (`ext2_ro_forced`), each logged
  by bit. All metadata goes through a block-device mapping (`filemap_bdev_init`), whose page locks rank at the
  new `LOCK_LEVEL_META_PAGE` (35, under a file page's 30). The block map caches the last last-level indirect
  block per inode (`ind_cache`, one atomic word: first file block << 32 | disk block), so sequential reads cost
  one cached-page lookup per block. File pages: holes zeroed, runs of contiguous blocks one piece each, and the
  block holding EOF copied from the metadata cache with its tail zeroed: with async pieces (M1-10), zeroing a
  tail that a piece in flight also covers would race the DMA. Device/FIFO/socket inodes stat fine, open gives
  -ENXIO (no device-number registry yet). A corrupt entry (rec_len, block or inode number out of range, a size
  no block map reaches) is -EIO and logged, never a read past a block.
- **Boot mount** (`fs/sysmount.c`): system read-only at `/system`, the data partition's `apps`,
  `models`, `packs`, `home`, `var` with `vfs_mount_subdir`. Partitions are picked by type GUID only. The boot
  disk is the one Limine names (`BootInfo.boot_disk_guid`, new) **if** a driver serves it and it holds an arc_os
  partition; otherwise labels pick the disk and the type must still match. An ISO boot reports the image's own
  GPT GUID (seen in QEMU), and so does the bring-up ISO written to a stick, so the label path is the normal one
  until the installer writes real arc_os disks (ADR-0018 "when it can't"). `arc.data=PARTUUID=` must name a
  partition of the data type, else no data is mounted (no silent fallback). The lookups live in
  `drivers/blk_lookup.c` (moved out of blkdev.c so host suites link them; new `blk_find_disk`,
  `blk_find_partuuid`; `blk_find_label` takes a type GUID, NULL = any, so a label alone never picks a partition
  arc_os mounts). The boot-time page waits poll the disks (#43's `blk_poll_all` in `page_wait_unlocked`).
- File pages after #43: `readpages` puts every page's pieces on one plug; the EOF block still comes from the
  metadata cache, so the EOF page is complete before any interrupt (host test with deferred requests).
- Tests: `test_ext2` (mkfs.ext2 images at 4/2/1 KiB blocks: 5 GiB sparse file read at every block-map level to
  triple indirect, holes, fast/slow symlinks, a 400-entry `dir_index` directory listed whole and in resumed
  batches, refused features, corrupt entries, failed mounts and OOM leak-free); a ramfs walk test (`..`, a renamed
  base, getcwd, rename ancestry, dot-name errnos); `boot_disk_guids` (bootinfo); `test_sysmount` (pick rules over
  `blk_mem_devices`, the host registry stand-in in `tests/blk_mem.c`) and `qemu.ext2` / `qemu.ext2.named` (two disks: a foreign one with arc_os *labels* on Linux and Windows
  partition types, attached writable, and an arc_os one; CRC-32 of every file checked against the host's, exec
  from ext2, foreign disk SHA-256 unchanged; a foreign PARTUUID refused). Cost: lookups scan a directory
  linearly (O(entries), htree ignored); a block map walk is O(depth), O(1) on the cached indirect block; mount
  O(groups); picking partitions O(devices).
- For the next blocks: M1-15 writes must clear `ind_cache` when a file's block map changes, keep `ro_forced`
  volumes read-only, clear `EXT2_INDEX_FL` on a `dir_index` directory it changes (it doesn't maintain the htree;
  my read, verify against e2fsprogs), and keep the EOF block's metadata-cache copy coherent with the file page
  (or read that block another way), since `ext2_start_page` copies it from there. M1-16 (`CLONE_FS`) adds the
  cwd's lock; today only the process's own thread touches it. No `ETXTBSY` yet. kmain still mounts
  vda's ESP at `/disk` even when vda isn't the boot disk (FAT32 read-only, from M1-10): ADR-0018 wants only the
  boot disk's; the bring-up block that next touches `disk_mount` should pick the ESP by the boot disk too. With a
  data partition, `/home` is its `home` directory, so the installer must create `home/user` (the ramfs one is
  hidden). Kernel image 558053 bytes (+19779 over #43's 538274), limit 586000. Two disks with the same GPT GUID
  (a cloned stick plugged in beside the original) are ambiguous: the first registered wins (my read: the
  installer should write fresh GUIDs).
