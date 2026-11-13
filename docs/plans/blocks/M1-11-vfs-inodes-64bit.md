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
- [x] Suites pass; no `uint32_t` offsets or sizes remain in `kernel/fs/` (FAT32's on-disk fields and in-cluster
  offsets stay 32-bit, as the format defines them).

**Cost.** Path lookup O(components) through a hashed dentry cache; inode, dentry and file from slab caches with size asserts; no scan of the mount table per component beyond mount points.

**Watch out.**
- Path resolution stays in one place (`path.c`); no filesystem parses paths itself.
- L block: a clean stopping point is after the core types + ramfs + devfs + pipe + tty are converted and passing.

**Notes.**
- From M1-01: procfs inode numbers overlap. `meminfo` is 2001 and `/proc/<pid>` is 2000 + pid, so it shares a
  number with `/proc/1`; `/proc/cpuinfo` took 1998 to stay clear. 64-bit inodes should give procfs a real scheme.
- From M1-05: the VFS, ramfs, devfs, procfs, FAT32 and fd tables have no locks of their own; the interim big
  kernel lock (held for every syscall) covers them. The inode lock is level 10 in `docs/kernel/locking.md`;
  this block takes the VFS out from under the kernel lock and updates that file's list. Pipes have their own
  row there: `pipe.c` moves bytes and checks reader/writer counts outside `pipe->lock`, and a writer's wakeup
  doesn't take it, so pipe I/O needs those under `pipe->lock` (or a lost wakeup and head/tail races follow)
  before the kernel lock comes off it. `/proc/sched_debug` took inode 1995.
- 2026-11-13 (built). `vfs.h`: `Inode` (64-bit size and ino, timespec times, `RwSem` lock, `mapping` for M1-12),
  `SuperBlock` (type, root, bdev, flags, rename lock, `st_dev`), `File` (64-bit pos under `pos_lock`, status
  flags, refcount) and `IoBuf` (one read/write path for kernel and user buffers). `inode.c` (lifetime, inode
  cache, permissions, stat), `dcache.c` (bounded LRU, positive entries only), `file.c` (open and I/O),
  `vfs.c` (path walk, 16-entry mount table with bind mounts, namespace changes, `/proc/mounts`). `fd.c`: refcounted
  tables, ctz bitmaps, close-on-exec. Every filesystem converted; `VfsNode` is gone. Syscalls: `open`/`openat`
  with all the flags, `pread64`/`pwrite64`, `ftruncate`, `stat`/`lstat`/`fstat`/`newfstatat`, `fcntl`, `dup3`,
  `pipe2`, `getdents64` (no entry limit), `rename`/`renameat`, `mkdirat`, `rmdir`, `unlinkat`, `fsync`.
- Locks: see `docs/kernel/locking.md` "VFS locks". The VFS, fd tables and pipes no longer need the kernel lock,
  but it is still taken at every syscall entry for the other rows, so the VFS locks have only run under it (and
  in the host tests); the first real concurrency comes when the last row goes.
- Tests: host runner `test_vfs_runner` builds the real `kernel/fs` sources, `fd.c` and `path.c` against
  `tests/vfs_env.c` (allocators with leak counters and failure injection, faulting user copies): vfs, fd, pipe,
  devfs, procfs, fat32 (an mtools image) and cred suites. QEMU `qemu.vfs` runs `fstest` (musl) and mounts the
  FAT32 test image as a read-only virtio-blk disk (`qemu-test.py --disk`). Bind mounts are host-tested only (no
  mount syscall yet; M1-13 makes the first real ones).
- Decisions: FAT32 is read-only (`SB_RDONLY`, writes give `EROFS`), as the spec says; the ESP is written only by
  the installer. Linux semantics where the old code differed: `unlink` of a directory is `EISDIR` (libarc gained
  `rmdir`, `rm -d`), `open` honours the mode (libarc's `open` read a mode it wasn't given; callers pass 0666),
  exec needs a regular file with an x bit (`EACCES`), an open that fails after creating leaves the file. Boot
  modules are 0755 and `/tmp` is 01777. Socket fds are Files on anonymous `S_IFSOCK` inodes (one reference each).
  The QEMU leak checks write 2 to `/proc/sys/vm/drop_caches` before they measure (dentries and the disk inodes
  they hold are a cache, not a leak); M1-12 makes 1 drop clean page-cache pages for the same reason.
- Next blocks: M1-12 moves ramfs data (a sparse page tree today) into `inode->mapping`, adds `readpage` (FAT32's
  `io_lock` is level 45 so it can sit under the page lock), takes the exec size limit (`exec_read_elf` still
  reads the whole file into one kmalloc) and drop_caches 1. M1-13 adds symlinks: `path_normalize` resolves ".."
  by text before the walk, which is wrong once a component can be a symlink, so the walk must follow links and
  handle ".." itself (`lstat` and `O_NOFOLLOW`/`AT_SYMLINK_NOFOLLOW` are no-ops until then). M1-13's subtree
  mounts use `vfs_bind_mount`; a bind marks the covered directory, so a mount below the bound subtree shows at
  both paths. M1-10 rebases `fat32.c` (its three `dev->read` calls). M1-16: pipe and tty sleeps can't be
  interrupted by signals yet; fd tables are refcounted for threads already. M1-18: socket fds use
  `socket_fops`; poll goes in `FileOps`.
- Open: the FAT is read whole into one kmalloc at mount, so a FAT over 4 MiB (more than ~1M clusters) refuses to
  mount (fine for an ESP). A tty read into a bad user buffer still loses the bytes (TTY row, lane KB). Inode
  times are uptime until M1-06 gives wall-clock time.
