#ifndef ARCHOS_FS_VFS_H
#define ARCHOS_FS_VFS_H

/* Virtual file system (M1-11): inodes, superblocks, a dentry cache, open
 * files and mounts, with 64-bit sizes and offsets.
 *
 *   SuperBlock  one mounted filesystem instance (ramfs, devfs, procfs, FAT32)
 *   Inode       one file, directory or device; refcounted; its filesystem
 *               fills the ops tables and owns `priv`
 *   File        one open file: inode, 64-bit position, status flags; shared
 *               by dup and fork through its refcount
 *   dcache      (parent inode, name) -> child inode, so a path lookup costs a
 *               hash probe per component (dcache.c)
 *   mounts      a filesystem's root, or any directory of one (a bind-style
 *               subtree mount), placed over an existing directory (vfs.c)
 *
 * Paths reaching this layer are as the caller gave them: absolute, or
 * relative to a starting directory (the cwd or a dirfd, as a PathLoc). The
 * walk (vfs.c) resolves ".", ".." (through the filesystem's own ".." entry,
 * and across mounts) and symlinks (M1-13); no filesystem parses paths.
 *
 * Locks, outermost first (docs/kernel/locking.md):
 *   File.pos_lock     sleeping, LOCK_LEVEL_FILE_POS: position of a seekable file
 *   rename_mutex      sleeping, LOCK_LEVEL_RENAME: one rename at a time, held
 *                     from before path resolution (vfs.c)
 *   Inode.lock        sleeping read-write, LOCK_LEVEL_INODE: a directory's
 *                     entries (lookup shared, changes exclusive) or a regular
 *                     file's data and size (read shared, write/truncate
 *                     exclusive). Rename takes the second directory at
 *                     LOCK_LEVEL_INODE_2 and a replaced or removed directory at
 *                     LOCK_LEVEL_INODE_CHILD.
 *   mount, dcache, inode-cache and fd-table spinlocks: leaves (nothing but the
 *                     slab allocator is taken under them)
 * User memory may be touched under the sleeping locks (the fault path takes
 * only mm locks, which come after them), never under the spinlocks. */

#include <stddef.h>
#include <stdint.h>
#include "uapi/fcntl.h"
#include "uapi/errno.h"
#include "uapi/stat.h"
#include "uapi/time.h"
#include "proc/mutex.h"
#include "proc/rwsem.h"

typedef int64_t loff_t;

/* Longest name of one path component, excluding the NUL (Linux NAME_MAX) */
#define VFS_NAME_MAX 255

/* Largest file size and offset (Linux MAX_LFS_FILESIZE) */
#define VFS_OFFSET_MAX INT64_MAX

/* lseek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Access wanted, for permission checks (access(2) values) */
#define R_OK 4
#define W_OK 2
#define X_OK 1

typedef struct Inode Inode;
struct VmaOps;
typedef struct SuperBlock SuperBlock;
typedef struct File File;
typedef struct DirContext DirContext;
typedef struct Mount Mount;

/* ---------------------------------------------------------------------------
 * Data buffers: a read puts bytes into, and a write takes them from, either
 * kernel memory or a user buffer. Filesystems copy through iob_* only, so the
 * same code serves read(2) (straight to user memory, no bounce buffer) and the
 * kernel's own reads (exec, boot modules).
 * ------------------------------------------------------------------------- */
typedef struct IoBuf {
    uint8_t *base;
    size_t len;
    int user;   /* 1: base is a user address (copy_to_user / copy_from_user) */
} IoBuf;

/* Copy n bytes from src to offset off of b. 0, or -EFAULT. */
int iob_copy_out(const IoBuf *b, size_t off, const void *src, size_t n);
/* Copy n bytes from offset off of b to dst. 0, or -EFAULT. */
int iob_copy_in(void *dst, const IoBuf *b, size_t off, size_t n);
/* As iob_copy_in, but stops at the first faulting byte and returns the bytes
 * copied, leaving dst past them unchanged: for copies straight into file
 * data, where a write that faults half-way keeps what it copied (as Linux)
 * and must not zero the rest. */
size_t iob_copy_in_partial(void *dst, const IoBuf *b, size_t off, size_t n);
/* Zero n bytes at offset off of b (holes in sparse files). 0, or -EFAULT. */
int iob_zero(const IoBuf *b, size_t off, size_t n);

static inline IoBuf iob_kernel(void *p, size_t n) {
    IoBuf b = {(uint8_t *)p, n, 0};
    return b;
}

static inline IoBuf iob_user(uint64_t addr, size_t n) {
    IoBuf b = {(uint8_t *)(uintptr_t)addr, n, 1};
    return b;
}

/* ---------------------------------------------------------------------------
 * Directory iteration (getdents64). The filesystem walks its entries from
 * ctx->pos, a cookie it chose (0 and 1 are "." and ".."), and hands each to
 * dir_emit with the cookie that resumes after it. dir_emit returns 0 when the
 * caller's buffer is full: the entry was not taken and ctx->pos still points
 * at it.
 * ------------------------------------------------------------------------- */
struct DirContext {
    loff_t pos;
    /* 0 = taken; nonzero = full */
    int (*actor)(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next);
};

static inline int dir_emit(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    if (ctx->actor(ctx, name, len, ino, dtype, next) != 0) return 0;
    ctx->pos = next;
    return 1;
}

/* Emit "." (cookie 0) and ".." (cookie 1) if ctx->pos is still below 2.
 * Returns 1 when both are done, 0 when the buffer filled. */
int dir_emit_dots(DirContext *ctx, uint64_t self_ino, uint64_t parent_ino);

/* DT_* type of a mode (Linux: the S_IFMT bits shifted down) */
static inline uint8_t vfs_dtype(uint32_t mode) {
    return (uint8_t)((mode & S_IFMT) >> 12);
}

/* ---------------------------------------------------------------------------
 * Operations. Return 0 or a negative errno unless noted. NULL entries are
 * handled by the VFS as noted.
 * ------------------------------------------------------------------------- */

/* Attribute change (chmod, chown, truncate) */
typedef struct InodeAttr {
    uint32_t valid;   /* ATTR_* */
    uint32_t mode;    /* permission bits only (07777) */
    uint32_t uid;
    uint32_t gid;
    loff_t size;
} InodeAttr;
#define ATTR_MODE 0x1
#define ATTR_UID  0x2
#define ATTR_GID  0x4
#define ATTR_SIZE 0x8

/* Called with the directory (dir, odir) locked: shared for lookup,
 * exclusive for the rest. Names are one NUL-terminated component. */
typedef struct InodeOps {
    /* Find name: 0 with *out (a new reference), or -ENOENT. ".." gives the
     * parent directory (the walk asks only below a filesystem's root, and
     * only for a directory that isn't I_DEAD); "." is never asked. */
    int (*lookup)(Inode *dir, const char *name, Inode **out);
    /* New regular file; mode includes S_IFREG. *out gets a reference.
     * NULL: -EACCES (as Linux's open(O_CREAT) on such a directory). */
    int (*create)(Inode *dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid, Inode **out);
    /* New directory; mode includes S_IFDIR. NULL: -EPERM. */
    int (*mkdir)(Inode *dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid);
    /* Remove a non-directory entry; victim is the inode it names. */
    int (*unlink)(Inode *dir, const char *name, Inode *victim);
    /* Remove an empty directory (victim locked at LOCK_LEVEL_INODE_CHILD). */
    int (*rmdir)(Inode *dir, const char *name, Inode *victim);
    /* Move odir/oname (inode src) to ndir/nname, replacing victim (NULL if
     * nname doesn't exist; an empty directory is locked). Both directories
     * are on this filesystem and locked; the VFS has checked the types. */
    int (*rename)(Inode *odir, const char *oname, Inode *src, Inode *ndir, const char *nname, Inode *victim);
    /* NULL: -EPERM (no filesystem creates symlinks yet: ext2 write is M1-15) */
    int (*symlink)(Inode *dir, const char *name, const char *target);
    /* A symlink's target: up to size bytes into buf (kernel memory, no NUL).
     * Returns the target's full length (may exceed size: the caller sees it
     * was cut) or -errno. Called with no lock held. */
    int (*readlink)(Inode *inode, char *buf, size_t size);
    /* Called with inode locked exclusive. NULL: mode/uid/gid are copied and
     * a size change gets -EPERM. */
    int (*setattr)(Inode *inode, const InodeAttr *attr);
} InodeOps;

/* FileOps.fflags */
#define FOP_STREAM 0x1   /* no position: pread/pwrite/lseek give -ESPIPE (pipes, sockets, terminals) */

typedef struct FileOps {
    uint32_t fflags;
    /* After a successful open, before the fd exists. NULL: nothing to do. */
    int (*open)(Inode *inode, File *file);
    /* The last reference to the File went away. */
    void (*release)(Inode *inode, File *file);
    /* Bytes moved (>= 0) or -errno. *pos advances by the bytes moved; stream
     * files ignore it. For a regular file the VFS holds the inode lock
     * (shared for read, exclusive for write). */
    int64_t (*read)(File *file, const IoBuf *buf, loff_t *pos);
    int64_t (*write)(File *file, const IoBuf *buf, loff_t *pos);
    /* NULL: the generic seek on the inode's size (vfs_generic_llseek). */
    loff_t (*llseek)(File *file, loff_t offset, int whence);
    /* NULL: -ENOTTY */
    int64_t (*ioctl)(File *file, uint64_t request, uint64_t arg);
    /* mmap: the operations of a VMA mapping this file (mm/vma.h; page-cache
     * files: filemap_vma_ops). NULL: mmap gives -ENODEV. */
    const struct VmaOps *mmap_ops;
    /* Placeholder: poll (M1-18). NULL today. */
    uint32_t (*poll)(File *file, void *table);
    /* NULL: nothing to write back, 0 */
    int (*fsync)(File *file);
    /* Directories: entries from ctx->pos on (dir locked shared). */
    int (*iterate)(File *file, DirContext *ctx);
} FileOps;

typedef struct SuperOps {
    /* The last reference to inode went away: free the filesystem's data for
     * it (inode->priv). The VFS frees the Inode afterwards. NULL: nothing. */
    void (*evict_inode)(Inode *inode);
} SuperOps;

/* ---------------------------------------------------------------------------
 * Objects
 * ------------------------------------------------------------------------- */

/* Inode.flags, changed only with atomic or/and */
#define I_STATIC     0x1   /* static storage: never freed (devfs, procfs fixed files) */
#define I_HASHED     0x2   /* in the inode cache */
#define I_MOUNTPOINT 0x4   /* a mount covers this directory */
#define I_DEAD       0x8   /* removed directory: no new entries */

struct Inode {
    uint64_t ino;
    uint32_t mode;    /* S_IFMT type bits | permission bits */
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint32_t rdev;    /* device number of a character device (Linux encoding) */
    uint32_t flags;   /* I_* */
    loff_t size;      /* bytes; 64-bit */
    uint64_t blocks;  /* 512-byte units allocated, as st_blocks */
    struct timespec atime;
    struct timespec mtime;
    struct timespec ctime;
    SuperBlock *sb;
    const InodeOps *iop;
    const FileOps *fop;
    void *mapping;    /* regular files: the AddressSpace of its page-cache pages (mm/filemap.h) */
    void *priv;       /* filesystem's own data */
    uint32_t refcount;   /* atomic; hashed inodes drop to 0 only under the inode-cache lock */
    Inode *hash_next;    /* inode-cache chain */
    RwSem lock;          /* LOCK_LEVEL_INODE */
};

/* SuperBlock.flags */
#define SB_NO_DCACHE 0x1   /* lookups are not cached (procfs: entries come and go with processes) */
#define SB_RDONLY    0x2   /* read-only: writes, creates, removes and setattr get -EROFS (FAT32) */

struct SuperBlock {
    const char *type;     /* "ramfs", "devfs", "proc", "vfat" (/proc/mounts) */
    uint32_t dev;         /* st_dev */
    uint32_t flags;       /* SB_* */
    Inode *root;
    const SuperOps *sop;
    void *bdev;           /* backing block device, or NULL */
    void *priv;           /* filesystem's own data */
};

/* Readahead state of an open file (mm/filemap.c). Windows in pages. */
typedef struct FileRa {
    uint32_t next;   /* page index a sequential reader asks for next */
    uint32_t size;   /* current window; 0 = none yet */
} FileRa;

struct File {
    Inode *inode;         /* a reference */
    const FileOps *fop;
    loff_t pos;           /* under pos_lock */
    uint32_t flags;       /* O_ACCMODE and the status flags (O_APPEND, O_NONBLOCK) */
    uint32_t refcount;    /* atomic: fd-table entries and callers in flight */
    Mount *mnt;           /* the mount the file was opened in (a dirfd's walks start here); NULL for pipes */
    void *priv;           /* the file's own data (socket index, ...) */
    FileRa ra;            /* readahead (racy by design: a hint shared by the File's users) */
    Mutex pos_lock;       /* LOCK_LEVEL_FILE_POS */
};

/* ---------------------------------------------------------------------------
 * Setup and inodes (inode.c)
 * ------------------------------------------------------------------------- */

/* Create the slab caches and the anonymous filesystem. Call once at boot. */
void vfs_init(void);

/* Initialise sb and give it a device number. */
void vfs_sb_init(SuperBlock *sb, const char *type, const SuperOps *sop);

/* Superblock of pipes and sockets (no mount) */
SuperBlock *vfs_anon_sb(void);

/* A new inode number on the anonymous superblock (one counter for pipes and
 * sockets, so no two share st_dev/st_ino). */
uint64_t vfs_anon_ino(void);

/* New inode from the slab: refcount 1, not hashed, nlink 1, times now. NULL
 * when out of memory. */
Inode *inode_new(SuperBlock *sb, uint64_t ino, uint32_t mode);

/* Set up a statically allocated inode (I_STATIC, never freed). */
void inode_init_static(Inode *inode, SuperBlock *sb, uint64_t ino, uint32_t mode);

/* Take a reference on an inode the caller already holds one on. Returns it. */
Inode *igrab(Inode *inode);

/* Drop a reference. The last one unhashes the inode, calls the filesystem's
 * evict_inode and frees it (not I_STATIC ones). Never under a spinlock. */
void iput(Inode *inode);

/* Inode cache, one per kernel, keyed by (sb, ino): filesystems whose inodes
 * come and go (FAT32) find a live inode here instead of making a second one.
 * icache_get returns it with a new reference, or NULL. icache_add hashes a
 * new inode (refcount 1, from inode_new) and returns it, or, when another
 * thread hashed the same (sb, ino) first, drops the new one and returns that
 * one with a reference. */
Inode *icache_get(SuperBlock *sb, uint64_t ino);
Inode *icache_add(Inode *inode);

/* Current time for inode times. Uptime until wall-clock time exists (M1-06). */
void vfs_now(struct timespec *ts);

/* Permission check for uid/gid (0 = root passes). 0 or -EACCES. */
int vfs_check_perm(const Inode *inode, uint32_t uid, uint32_t gid, int want);

/* The same with the calling process's effective uid and gid. */
int inode_permission(const Inode *inode, int want);

/* Fill a Linux struct stat from an inode. */
void vfs_fill_stat(const Inode *inode, struct stat *st);

/* ---------------------------------------------------------------------------
 * Mounts and names (vfs.c). Paths are absolute and normalized.
 * ------------------------------------------------------------------------- */

/* Where a path ended: the mount it is in and the inode (a reference). A
 * walk starts at one too (the cwd, a dirfd). */
typedef struct PathLoc {
    Mount *mnt;
    Inode *inode;
} PathLoc;

/* Walk flags */
#define WALK_FOLLOW 0x1   /* follow a symlink in the last component (stat, open; not lstat, O_NOFOLLOW) */

/* Symlinks one walk may follow (Linux MAXSYMLINKS); one more is -ELOOP */
#define VFS_MAX_LINKS 40

/* Make sb the root filesystem ("/"). Once, before any other mount. */
int vfs_mount_root(SuperBlock *sb, const char *source);

/* Mount sb's root over the existing directory at path. -ENOTDIR, -ENOENT,
 * -EBUSY (already a mount point), -ENOMEM (table full). */
int vfs_mount(const char *path, SuperBlock *sb, const char *source);

/* Mount the directory at src (any directory of a mounted filesystem) over
 * the directory at target too: the subtree is reachable at both paths. */
int vfs_bind_mount(const char *src, const char *target);

/* Mount the directory `name` (one component) of sb's root over the existing
 * directory at target: a subtree mount of a filesystem that has no mount of
 * its own (the data partition's /apps, /home, ...; ADR-0009). */
int vfs_mount_subdir(const char *target, SuperBlock *sb, const char *name, const char *source);

/* /proc/mounts text ("source target type rw 0 0" lines) into buf. Returns
 * the length; output stops at size - 1. */
int vfs_format_mounts(char *buf, int size);

/* Resolve path, relative to base (NULL: "/") unless it starts with '/'.
 * 0 with *out filled (release with loc_put), or -errno: -ENOENT (also for
 * ""), -ENOTDIR, -EACCES, -ENAMETOOLONG, -ELOOP. A trailing '/' requires a
 * directory (and follows a final symlink). */
int vfs_resolve_at(const PathLoc *base, const char *path, uint32_t flags, PathLoc *out);

static inline int vfs_resolve(const char *path, PathLoc *out) {
    return vfs_resolve_at(NULL, path, WALK_FOLLOW, out);
}

/* Copy a PathLoc, taking a reference. */
void loc_get(PathLoc *dst, const PathLoc *src);

/* Release a PathLoc's inode reference. */
void loc_put(PathLoc *loc);

/* Resolve all but the last component of path: *dir is the directory (a
 * reference), name its last component (".", ".." included: callers refuse
 * them as Linux does), *slash 1 when the path ends in '/'. -EINVAL for "/"
 * itself, -ENOENT, -ENOTDIR, -ENAMETOOLONG, -ELOOP. */
int vfs_resolve_parent_at(const PathLoc *base, const char *path, PathLoc *dir, char name[VFS_NAME_MAX + 1], int *slash);

/* The absolute path of a directory (getcwd): from loc up to "/" through
 * "..", each name found by a scan of its parent; a mount root takes its
 * mount's path. Returns the length (without NUL), -ENOENT for a removed
 * directory, -ERANGE when it doesn't fit size. O(depth x entries). */
int vfs_path_of(const PathLoc *loc, char *buf, size_t size);

/* Namespace operations by path (relative to base, NULL: "/"), with the
 * calling process's credentials. */
int vfs_mkdir_at(const PathLoc *base, const char *path, uint32_t mode);
int vfs_unlink_at(const PathLoc *base, const char *path);
int vfs_rmdir_at(const PathLoc *base, const char *path);
int vfs_rename_at(const PathLoc *obase, const char *oldpath, const PathLoc *nbase, const char *newpath);
int vfs_stat_at(const PathLoc *base, const char *path, uint32_t flags, struct stat *st);
/* readlink(2): the target of the symlink at path (not followed), up to
 * size bytes. Bytes copied, -EINVAL if not a symlink. */
int vfs_readlink_at(const PathLoc *base, const char *path, char *buf, size_t size);

static inline int vfs_mkdir(const char *path, uint32_t mode) {
    return vfs_mkdir_at(NULL, path, mode);
}
static inline int vfs_unlink(const char *path) {
    return vfs_unlink_at(NULL, path);
}
static inline int vfs_rmdir(const char *path) {
    return vfs_rmdir_at(NULL, path);
}
static inline int vfs_rename(const char *oldpath, const char *newpath) {
    return vfs_rename_at(NULL, oldpath, NULL, newpath);
}
static inline int vfs_stat(const char *path, struct stat *st) {
    return vfs_stat_at(NULL, path, WALK_FOLLOW, st);
}

/* The calling process's working directory (a reference; "/" for the kernel
 * and for processes that never changed it), and changing it. A process's
 * cwd is touched only by its own thread until threads share one (CLONE_FS,
 * M1-16, adds a lock). */
void vfs_cwd_get(PathLoc *out);
void vfs_cwd_set(PathLoc *loc);   /* takes over loc's reference */
struct Process;
/* fork: the child starts in the parent's directory. exit: drop it. */
void vfs_cwd_copy(struct Process *child, const struct Process *parent);
void vfs_cwd_release(struct Process *p);

/* chmod (ATTR_MODE), chown (ATTR_UID/ATTR_GID, root only) and truncate
 * (ATTR_SIZE) on an inode, with permission checks and the inode locked. */
int vfs_setattr(Inode *inode, const InodeAttr *attr);

/* ---------------------------------------------------------------------------
 * Open files (file.c)
 * ------------------------------------------------------------------------- */

/* Open path relative to base (O_CREAT with mode, O_EXCL, O_TRUNC, O_APPEND,
 * O_DIRECTORY, O_NOFOLLOW, O_NONBLOCK; O_CLOEXEC is the caller's, for the
 * fd). *out is a File with one reference. */
int vfs_open_at(const PathLoc *base, const char *path, uint32_t flags, uint32_t mode, File **out);

/* Open path for execve: a regular file with execute permission (-EACCES
 * otherwise); read permission is not needed, as on Linux. Read-only File. */
int vfs_open_exec_at(const PathLoc *base, const char *path, File **out);

static inline int vfs_open(const char *path, uint32_t flags, uint32_t mode, File **out) {
    return vfs_open_at(NULL, path, flags, mode, out);
}
static inline int vfs_open_exec(const char *path, File **out) {
    return vfs_open_exec_at(NULL, path, out);
}

/* A File on an inode the caller holds a reference for (taken over by the
 * File). For pipes and sockets. NULL when out of memory. */
File *file_new(Inode *inode, const FileOps *fop, uint32_t flags);

/* Take / drop a File reference. The last fput calls release, drops the
 * inode and frees the File. */
File *fget_ref(File *file);
void fput(File *file);

/* read/write at the File's position (advanced), and at an explicit one
 * (pread/pwrite: -ESPIPE on a stream, -EINVAL for a negative offset). */
int64_t vfs_read(File *file, const IoBuf *buf);
int64_t vfs_write(File *file, const IoBuf *buf);
int64_t vfs_pread(File *file, const IoBuf *buf, loff_t pos);
int64_t vfs_pwrite(File *file, const IoBuf *buf, loff_t pos);

/* lseek. The new position, or -EINVAL / -ESPIPE / -EOVERFLOW. */
loff_t vfs_llseek(File *file, loff_t offset, int whence);

/* The seek every seekable file gets when its llseek is NULL. */
loff_t vfs_generic_llseek(File *file, loff_t offset, int whence);

/* Directory entries from the File's position on (getdents64). */
int vfs_iterate(File *file, DirContext *ctx);

/* ftruncate: needs the file open for writing. */
int vfs_ftruncate(File *file, loff_t length);

int vfs_fsync(File *file);

/* Empty the dentry cache, freeing the disk inodes only it held
 * (/proc/sys/vm/drop_caches; the QEMU suites' leak checks drop caches before
 * they measure, so cached entries don't count as leaks). O(entries); entries
 * added meanwhile may stay. Call with no spinlock held. It may run under the
 * caller's own inode lock (a write to drop_caches holds that file's), so an
 * evict_inode must never take an inode lock. */
void vfs_drop_caches(void);

/* Whole-file helper for the kernel: create (or truncate) path with mode and
 * write size bytes. 0 or -errno. */
int vfs_write_file(const char *path, const void *data, uint64_t size, uint32_t mode);

#endif /* ARCHOS_FS_VFS_H */
