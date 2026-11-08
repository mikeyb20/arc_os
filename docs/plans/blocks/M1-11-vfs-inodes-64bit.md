# M1-11 — VFS rework: inodes, open files, 64-bit offsets

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M0-06 | M1-12, M1-13, M1-18, M1-20 |

**Goal.** The VFS uses inodes, superblocks and open-file objects with 64-bit sizes and offsets, ready for the page
cache and ext2. Today's `VfsNode` read/write take 32-bit offsets, which caps files at 4 GiB.

**Read first.**
- `kernel/fs/vfs.c`, `vfs.h`, `ramfs.c`, `devfs.c`, `procfs.c`, `fat32.c`, `pipe.c`, `path.c`
- `kernel/proc/fd.c`, `fd.h`, `kernel/drivers/tty.c`, `kernel/net/socket.c`
- `docs/decisions/0004-page-cache-and-mmap.md`, `0009-directory-layout.md`

**Build.**
1. `struct inode` (ino, mode, uid, gid, 64-bit size, nlink, timespec times, blocks, superblock, inode ops, file ops,
   mapping pointer for M1-12, private), `struct super_block` (type, root, block device, ops), a small dentry cache
   (name → inode, used by path lookup), and `struct file` (inode, 64-bit position, flags, refcount, ops) shared by
   `dup` and `fork`.
2. File ops: `read`/`write` with `(buf, size, loff_t *pos)`, `llseek`, `ioctl`, `mmap` and `poll` placeholders
   (M1-12, M1-18), `fsync`, and directory iteration through a `getdents64`-style callback.
3. Inode ops: lookup, create, mkdir, unlink, rmdir, rename, symlink/readlink (may return `-EPERM` where a filesystem
   lacks them), setattr (chmod, chown, truncate).
4. Mount table: mount a filesystem at a path, and mount a **subtree** of a mounted filesystem at another path
   (bind-style), so one data partition can serve `/apps /models /packs /home /var` (ADR-0009). `/proc/mounts`.
5. Convert ramfs, devfs, procfs, fat32 (read-only), pipe, tty and sockets one at a time, building and testing after
   each; then delete the old `VfsNode` API.
6. Syscalls on the new VFS: `open`/`openat` (`O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_CLOEXEC`,
   `O_DIRECTORY`, `O_NONBLOCK`), 64-bit `lseek`, `pread64`, `pwrite64`, `ftruncate`, `fstat`, `stat`, `lstat`,
   `newfstatat`, `fcntl` (`F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`, `F_DUPFD`, `F_DUPFD_CLOEXEC`), `dup3`,
   `getdents64`, `rename`/`renameat`, `mkdirat`, `unlinkat`.
7. Tests: all existing filesystem tests; `lseek`/`pread` beyond 4 GiB on a sparse ramfs file; shared offset after
   `dup` and `fork`; `O_APPEND`; rename across directories; a bind-style subtree mount.

**Done when.**
- [ ] Suites pass; no `uint32_t` offsets or sizes remain in `kernel/fs/`.

**Cost.** Path lookup O(components) through a hashed dentry cache; inode, dentry and file from slab caches with size asserts; no scan of the mount table per component beyond mount points.

**Watch out.**
- Path resolution stays in one place (`path.c`); no filesystem parses paths itself.
- L block: a clean stopping point is after the core types + ramfs + devfs + pipe + tty are converted and passing.

**Notes.**
- From M1-01: procfs inode numbers overlap. `meminfo` is 2001 and `/proc/<pid>` is 2000 + pid, so it shares a
  number with `/proc/1`; `/proc/cpuinfo` took 1998 to stay clear. 64-bit inodes should give procfs a real scheme.
