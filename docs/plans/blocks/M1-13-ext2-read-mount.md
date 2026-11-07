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
- [ ] Host and QEMU tests pass.

**Watch out.**
- For regular files, `i_size_high` (old name `i_dir_acl`) holds the top 32 bits of the size.
- ext2 block numbers are 32-bit; that is fine for these disk sizes.
- Writing is M1-15. Mount read-only here.

**Notes.**
-
