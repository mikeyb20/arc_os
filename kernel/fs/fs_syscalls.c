/* arc_os — file and directory syscalls (Linux x86_64 numbers, ADR-0001).
 *
 * Owns: the syscall layer of the VFS: fds to Files, user paths (cwd- or
 * dirfd-relative) to absolute ones, and the Linux argument and errno rules.
 * Handlers return -errno on failure. Data moves straight between user memory
 * and the filesystem through IoBuf (vfs.h); no bounce buffer.
 *
 * Every handler that uses an fd takes a File reference (fd_get) and drops it
 * before returning, so a concurrent close can't free the File under it.
 * fds 0-2 with nothing in them still mean the terminal (devfs_console) for
 * read, write and ioctl, for processes started without a table entry.
 *
 * Locks: none of its own; see vfs.h and fd.h. */

#include "fs/fs_syscalls.h"
#include "arch/hal.h"
#include "fs/vfs.h"
#include "fs/pipe.h"
#include "fs/path.h"
#include "fs/devfs.h"
#include "proc/process.h"
#include "proc/fd.h"
#include "proc/signal.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "user_access.h"
#include "uapi/unistd.h"
#include "uapi/stat.h"
#include "uapi/dirent.h"
#include "uapi/uio.h"

/* Largest transfer one read or write makes (Linux MAX_RW_COUNT) */
#define MAX_RW_COUNT 0x7FFFF000ULL

/* readv/writev: at most this many iovecs (Linux UIO_MAXIOV), copied in
 * batches of IOV_BATCH (kernel stack: 32 x 16 B) */
#define IOV_MAX_COUNT 1024
#define IOV_BATCH     32

/* getdents64 record: name offset and alignment */
#define DIRENT_NAME_OFF 19

/* --- fds and paths --- */

static FdTable *cur_fds(void) {
    Process *p = proc_current();
    return p != NULL ? p->fd_table : NULL;
}

/* The File at fd (a reference), or NULL */
static File *get_file(int64_t fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return fd_get(cur_fds(), (int)fd);
}

/* get_file, but fds 0-2 with nothing in them are the terminal */
static File *get_stdio_file(int64_t fd) {
    File *f = get_file(fd);
    if (f == NULL && fd >= 0 && fd <= 2) f = devfs_console();
    return f;
}

/* The *at calls find dirfd's directory by the path it was opened by (the
 * walk starts from a path, not an inode; M1-13 changes that). If that path no
 * longer leads to the same directory (renamed or removed since), refuse with
 * -ENOENT rather than act in whatever now has that name. */
static int dir_path_current(File *dir) {
    PathLoc loc;
    if (vfs_resolve(dir->path, &loc) != 0) return 0;
    int same = loc.inode == dir->inode;
    loc_put(&loc);
    return same;
}

/* Copy a user path and make it absolute against dirfd (AT_FDCWD: the cwd).
 * An empty path gives -ENOENT, or 1 with AT_EMPTY_PATH allowed (the caller
 * then works on dirfd itself). */
static int at_path(int64_t dirfd, uint64_t path_addr, char *abs, size_t size, int allow_empty) {
    char path[PATH_MAX];
    long len = strncpy_from_user(path, (const char *)path_addr, sizeof(path));
    if (len < 0) return (int)len;   /* -EFAULT or -ENAMETOOLONG */
    if (len == 0) return allow_empty ? 1 : -ENOENT;
    Process *p = proc_current();
    if (path[0] == '/' || (int32_t)dirfd == AT_FDCWD) {
        return path_normalize(p != NULL ? p->cwd : "/", path, abs, size);
    }
    File *dir = get_file(dirfd);
    if (dir == NULL) return -EBADF;
    int err = (!S_ISDIR(dir->inode->mode) || dir->path == NULL) ? -ENOTDIR : path_normalize(dir->path, path, abs, size);
    if (err == 0 && !dir_path_current(dir)) err = -ENOENT;
    fput(dir);
    return err;
}

int fs_user_path(uint64_t path_addr, char *abs, size_t size) {
    return at_path(AT_FDCWD, path_addr, abs, size, 0);
}

/* --- read and write --- */

static int64_t rw_file(File *f, uint64_t buf_addr, uint64_t count, int write) {
    if (count > MAX_RW_COUNT) count = MAX_RW_COUNT;
    if (count != 0 && !user_ptr_valid((const void *)buf_addr, count)) return -EFAULT;
    IoBuf b = iob_user(buf_addr, count);
    int64_t r = write ? vfs_write(f, &b) : vfs_read(f, &b);
    if (r == -EPIPE) sig_send(proc_current()->pid, SIGPIPE);
    return r;
}

/* read(fd, buf, count) — 0 */
static int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    File *f = get_stdio_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    int64_t r = rw_file(f, buf_addr, count, 0);
    fput(f);
    return r;
}

/* write(fd, buf, count) — 1 */
static int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    File *f = get_stdio_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    int64_t r = rw_file(f, buf_addr, count, 1);
    fput(f);
    return r;
}

/* readv/writev: one transfer per iovec, in order. Stops at the first error
 * or short transfer; returns the bytes moved, or the error if none moved. */
static int64_t do_rw_vector(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt, int write) {
    if ((int64_t)iovcnt < 0 || iovcnt > IOV_MAX_COUNT) return -EINVAL;
    File *f = get_stdio_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    struct iovec iov[IOV_BATCH];
    int64_t total = 0, err = 0;
    int stop = 0;
    for (uint64_t done = 0; done < iovcnt && !stop; done += IOV_BATCH) {
        uint64_t n = iovcnt - done < IOV_BATCH ? iovcnt - done : IOV_BATCH;
        if (copy_from_user(iov, (const void *)(iov_addr + done * sizeof(struct iovec)), n * sizeof(struct iovec))) {
            err = -EFAULT;
            break;
        }
        for (uint64_t i = 0; i < n && !stop; i++) {
            uint64_t len = iov[i].iov_len;
            if (len > MAX_RW_COUNT - (uint64_t)total) len = MAX_RW_COUNT - (uint64_t)total;
            if (len == 0) continue;
            int64_t r = rw_file(f, (uint64_t)iov[i].iov_base, len, write);
            if (r < 0)
                err = r;
            else
                total += r;
            stop = r < 0 || (uint64_t)r < len;   /* an error or a short transfer ends the call */
        }
    }
    fput(f);
    return total > 0 ? total : err;
}

/* readv(fd, iov, iovcnt) — 19 */
static int64_t sys_readv(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    return do_rw_vector(fd, iov_addr, iovcnt, 0);
}

/* writev(fd, iov, iovcnt) — 20. musl's stdio writes through this. */
static int64_t sys_writev(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    return do_rw_vector(fd, iov_addr, iovcnt, 1);
}

static int64_t prw(uint64_t fd, uint64_t buf_addr, uint64_t count, uint64_t pos, int write) {
    File *f = get_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    if (count > MAX_RW_COUNT) count = MAX_RW_COUNT;
    int64_t r;
    if (count != 0 && !user_ptr_valid((const void *)buf_addr, count)) {
        r = -EFAULT;
    } else {
        IoBuf b = iob_user(buf_addr, count);
        r = write ? vfs_pwrite(f, &b, (loff_t)pos) : vfs_pread(f, &b, (loff_t)pos);
    }
    fput(f);
    return r;
}

/* pread64(fd, buf, count, offset) — 17 */
static int64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t pos, uint64_t a4, uint64_t a5) {
    (void)a4, (void)a5;
    return prw(fd, buf, count, pos, 0);
}

/* pwrite64(fd, buf, count, offset) — 18 */
static int64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t pos, uint64_t a4, uint64_t a5) {
    (void)a4, (void)a5;
    return prw(fd, buf, count, pos, 1);
}

/* --- open, close, fds --- */

/* openat(dirfd, path, flags, mode) — 257 */
static int64_t sys_openat(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mode, uint64_t a4, uint64_t a5) {
    (void)a4, (void)a5;
    FdTable *t = cur_fds();
    if (t == NULL) return -EBADF;
    char abs[PATH_MAX];
    int err = at_path((int64_t)dirfd, path_addr, abs, sizeof(abs), 0);
    if (err != 0) return err;
    File *f;
    err = vfs_open(abs, (uint32_t)flags, (uint32_t)mode, &f);
    if (err != 0) return err;
    int fd = fd_install(t, f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0) fput(f);
    return fd;
}

/* open(path, flags, mode) — 2 */
static int64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3;
    return sys_openat((uint64_t)(int64_t)AT_FDCWD, path_addr, flags, mode, a4, a5);
}

/* close(fd) — 3 */
static int64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2, (void)a3, (void)a4, (void)a5;
    if ((int64_t)fd < 0 || fd >= MAX_FDS) return -EBADF;
    return fd_close(cur_fds(), (int)fd);
}

/* dup(oldfd) — 32: lowest free fd */
static int64_t sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2, (void)a3, (void)a4, (void)a5;
    File *f = get_file((int64_t)oldfd);
    if (f == NULL) return -EBADF;
    int fd = fd_install(cur_fds(), f, 0, 0);
    if (fd < 0) fput(f);
    return fd;
}

static int64_t dup_to(uint64_t oldfd, uint64_t newfd, int cloexec) {
    if (newfd >= MAX_FDS) return -EBADF;
    File *f = get_file((int64_t)oldfd);
    if (f == NULL) return -EBADF;
    int err = fd_install_at(cur_fds(), (int)newfd, f, cloexec);   /* closes what newfd held */
    if (err != 0) fput(f);
    return err != 0 ? err : (int64_t)newfd;
}

/* dup2(oldfd, newfd) — 33 */
static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    if (oldfd == newfd) {   /* Linux: newfd back if oldfd is open, else -EBADF */
        File *f = get_file((int64_t)oldfd);
        if (f == NULL) return -EBADF;
        fput(f);
        return (int64_t)newfd;
    }
    return dup_to(oldfd, newfd, 0);
}

/* dup3(oldfd, newfd, flags) — 292: dup2 with O_CLOEXEC; equal fds are -EINVAL */
static int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    if ((flags & ~(uint64_t)O_CLOEXEC) != 0 || oldfd == newfd) return -EINVAL;
    return dup_to(oldfd, newfd, (flags & O_CLOEXEC) != 0);
}

/* fcntl(fd, cmd, arg) — 72: F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD,
 * F_GETFL, F_SETFL (O_APPEND and O_NONBLOCK change; the rest are kept) */
static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    FdTable *t = cur_fds();
    File *f = get_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    int64_t r;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if ((int64_t)arg < 0 || arg >= MAX_FDS) {
            r = -EINVAL;
            break;
        }
        r = fd_install(t, fget_ref(f), (int)arg, cmd == F_DUPFD_CLOEXEC);
        if (r < 0) fput(f);   /* the reference fd_install didn't take */
        break;
    case F_GETFD:
        r = fd_get_cloexec(t, (int)fd);
        r = r < 0 ? r : (r ? FD_CLOEXEC : 0);
        break;
    case F_SETFD:
        r = fd_set_cloexec(t, (int)fd, (arg & FD_CLOEXEC) != 0);
        break;
    case F_GETFL:
        r = __atomic_load_n(&f->flags, __ATOMIC_RELAXED);
        break;
    case F_SETFL: {
        const uint32_t settable = O_APPEND | O_NONBLOCK;
        uint32_t old = __atomic_load_n(&f->flags, __ATOMIC_RELAXED);
        __atomic_store_n(&f->flags, (old & ~settable) | ((uint32_t)arg & settable), __ATOMIC_RELAXED);
        r = 0;
        break;
    }
    default:
        r = -EINVAL;
    }
    fput(f);
    return r;
}

/* pipe2(pipefd[2], flags) — 293: O_CLOEXEC and O_NONBLOCK */
static int64_t sys_pipe2(uint64_t pipefd_addr, uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    if ((flags & ~(uint64_t)(O_CLOEXEC | O_NONBLOCK)) != 0) return -EINVAL;
    if (!user_ptr_valid((void *)pipefd_addr, 2 * sizeof(int32_t))) return -EFAULT;   /* before creating */
    FdTable *t = cur_fds();
    if (t == NULL) return -EBADF;
    File *rd, *wr;
    int err = pipe_create(&rd, &wr, (uint32_t)flags);
    if (err != 0) return err;
    int cloexec = (flags & O_CLOEXEC) != 0;
    int rfd = fd_install(t, rd, 0, cloexec);
    if (rfd < 0) {
        fput(rd);
        fput(wr);
        return rfd;
    }
    int wfd = fd_install(t, wr, 0, cloexec);
    if (wfd < 0) {
        fd_close(t, rfd);
        fput(wr);
        return wfd;
    }
    int32_t fds[2] = {rfd, wfd};
    if (copy_to_user((void *)pipefd_addr, fds, sizeof(fds)) != 0) {
        fd_close(t, rfd);   /* Linux: the fds go again */
        fd_close(t, wfd);
        return -EFAULT;
    }
    return 0;
}

/* pipe(pipefd[2]) — 22 */
static int64_t sys_pipe(uint64_t pipefd_addr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1;
    return sys_pipe2(pipefd_addr, 0, a2, a3, a4, a5);
}

/* --- Positions, sizes, ioctl --- */

/* lseek(fd, offset, whence) — 8 */
static int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    File *f = get_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    int64_t r = vfs_llseek(f, (loff_t)offset, (int)whence);
    fput(f);
    return r;
}

/* ftruncate(fd, length) — 77 */
static int64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    File *f = get_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    int64_t r = vfs_ftruncate(f, (loff_t)length);
    fput(f);
    return r;
}

/* fsync(fd) — 74 */
static int64_t sys_fsync(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2, (void)a3, (void)a4, (void)a5;
    File *f = get_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    int64_t r = vfs_fsync(f);
    fput(f);
    return r;
}

/* fdatasync(fd) — 75: fsync (the cache keeps no metadata apart from the data) */
static int64_t sys_fdatasync(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return sys_fsync(fd, a1, a2, a3, a4, a5);
}

/* ioctl(fd, request, arg) — 16: the file's own requests (terminal ones on
 * /dev/tty), -ENOTTY for anything else */
static int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    File *f = get_stdio_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    int64_t r = (f->fop != NULL && f->fop->ioctl != NULL) ? f->fop->ioctl(f, request, arg) : -ENOTTY;
    fput(f);
    return r;
}

/* --- stat --- */

/* fstat(fd, statbuf) — 5 */
static int64_t sys_fstat(uint64_t fd, uint64_t stat_addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    File *f = get_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    struct stat st;
    vfs_fill_stat(f->inode, &st);
    fput(f);
    return copy_to_user((void *)stat_addr, &st, sizeof(st));
}

/* newfstatat(dirfd, path, statbuf, flags) — 262. No symlinks exist yet, so
 * AT_SYMLINK_NOFOLLOW changes nothing; AT_EMPTY_PATH stats dirfd itself. */
static int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr, uint64_t flags, uint64_t a4,
                              uint64_t a5) {
    if ((flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0) return -EINVAL;
    char abs[PATH_MAX];
    int err = at_path((int64_t)dirfd, path_addr, abs, sizeof(abs), (flags & AT_EMPTY_PATH) != 0);
    if (err == 1) {   /* empty path with AT_EMPTY_PATH: dirfd itself */
        if ((int32_t)dirfd != AT_FDCWD) return sys_fstat(dirfd, stat_addr, 0, 0, a4, a5);
        Process *p = proc_current();
        strncpy(abs, p != NULL ? p->cwd : "/", sizeof(abs));
        err = 0;
    }
    if (err != 0) return err;
    struct stat st;
    err = vfs_stat(abs, &st);
    if (err != 0) return err;
    return copy_to_user((void *)stat_addr, &st, sizeof(st));
}

/* stat(path, statbuf) — 4 */
static int64_t sys_stat(uint64_t path_addr, uint64_t stat_addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3;
    return sys_newfstatat((uint64_t)(int64_t)AT_FDCWD, path_addr, stat_addr, 0, a4, a5);
}

/* lstat(path, statbuf) — 6 */
static int64_t sys_lstat(uint64_t path_addr, uint64_t stat_addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3;
    return sys_newfstatat((uint64_t)(int64_t)AT_FDCWD, path_addr, stat_addr, AT_SYMLINK_NOFOLLOW, a4, a5);
}

/* --- Directories and names --- */

/* getcwd(buf, size) — 79: returns the length including the NUL */
static int64_t sys_getcwd(uint64_t buf_addr, uint64_t size, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOENT;
    size_t cwd_len = strlen(p->cwd) + 1;
    if (size < cwd_len) return -ERANGE;
    if (copy_to_user((void *)buf_addr, p->cwd, cwd_len) != 0) return -EFAULT;
    return (int64_t)cwd_len;
}

/* chdir(path) — 80 */
static int64_t sys_chdir(uint64_t path_addr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2, (void)a3, (void)a4, (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOENT;
    char abs[PATH_MAX];
    int err = fs_user_path(path_addr, abs, sizeof(abs));
    if (err != 0) return err;
    PathLoc loc;
    err = vfs_resolve(abs, &loc);
    if (err != 0) return err;
    if (!S_ISDIR(loc.inode->mode))
        err = -ENOTDIR;
    else
        err = inode_permission(loc.inode, X_OK);
    loc_put(&loc);
    if (err == 0) strncpy(p->cwd, abs, PATH_MAX);
    return err;
}

/* mkdirat(dirfd, path, mode) — 258 */
static int64_t sys_mkdirat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    char abs[PATH_MAX];
    int err = at_path((int64_t)dirfd, path_addr, abs, sizeof(abs), 0);
    return err != 0 ? err : vfs_mkdir(abs, (uint32_t)mode);
}

/* mkdir(path, mode) — 83 */
static int64_t sys_mkdir(uint64_t path_addr, uint64_t mode, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2;
    return sys_mkdirat((uint64_t)(int64_t)AT_FDCWD, path_addr, mode, a3, a4, a5);
}

/* unlinkat(dirfd, path, flags) — 263: AT_REMOVEDIR makes it rmdir */
static int64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    if ((flags & ~(uint64_t)AT_REMOVEDIR) != 0) return -EINVAL;
    char abs[PATH_MAX];
    int err = at_path((int64_t)dirfd, path_addr, abs, sizeof(abs), 0);
    if (err != 0) return err;
    return (flags & AT_REMOVEDIR) ? vfs_rmdir(abs) : vfs_unlink(abs);
}

/* unlink(path) — 87 */
static int64_t sys_unlink(uint64_t path_addr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2;
    return sys_unlinkat((uint64_t)(int64_t)AT_FDCWD, path_addr, 0, a3, a4, a5);
}

/* rmdir(path) — 84 */
static int64_t sys_rmdir(uint64_t path_addr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2;
    return sys_unlinkat((uint64_t)(int64_t)AT_FDCWD, path_addr, AT_REMOVEDIR, a3, a4, a5);
}

/* renameat(olddirfd, oldpath, newdirfd, newpath) — 264 */
static int64_t sys_renameat(uint64_t odfd, uint64_t old_addr, uint64_t ndfd, uint64_t new_addr, uint64_t a4,
                            uint64_t a5) {
    (void)a4, (void)a5;
    char oldp[PATH_MAX], newp[PATH_MAX];
    int err = at_path((int64_t)odfd, old_addr, oldp, sizeof(oldp), 0);
    if (err == 0) err = at_path((int64_t)ndfd, new_addr, newp, sizeof(newp), 0);
    return err != 0 ? err : vfs_rename(oldp, newp);
}

/* rename(oldpath, newpath) — 82 */
static int64_t sys_rename(uint64_t old_addr, uint64_t new_addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3;
    return sys_renameat((uint64_t)(int64_t)AT_FDCWD, old_addr, (uint64_t)(int64_t)AT_FDCWD, new_addr, a4, a5);
}

/* Change attributes of the inode at a user path. */
static int64_t path_setattr(uint64_t path_addr, const InodeAttr *attr) {
    char abs[PATH_MAX];
    int err = fs_user_path(path_addr, abs, sizeof(abs));
    if (err != 0) return err;
    PathLoc loc;
    err = vfs_resolve(abs, &loc);
    if (err != 0) return err;
    err = vfs_setattr(loc.inode, attr);
    loc_put(&loc);
    return err;
}

/* chmod(path, mode) — 90 */
static int64_t sys_chmod(uint64_t path_addr, uint64_t mode, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    InodeAttr attr = {.valid = ATTR_MODE, .mode = (uint32_t)mode & 07777};
    return path_setattr(path_addr, &attr);
}

/* chown(path, uid, gid) — 92: root only; -1 leaves a field unchanged */
static int64_t sys_chown(uint64_t path_addr, uint64_t uid, uint64_t gid, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    InodeAttr attr = {.uid = (uint32_t)uid, .gid = (uint32_t)gid};
    if ((uint32_t)uid != (uint32_t)-1) attr.valid |= ATTR_UID;
    if ((uint32_t)gid != (uint32_t)-1) attr.valid |= ATTR_GID;
    if (attr.valid == 0) {   /* nothing to change: still a permission and existence check */
        Process *p = proc_current();
        if (p == NULL || p->euid != 0) return -EPERM;
    }
    return path_setattr(path_addr, &attr);
}

/* umask(mask) — 95: returns the previous mask */
static int64_t sys_umask(uint64_t new_mask, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2, (void)a3, (void)a4, (void)a5;
    Process *p = proc_current();
    if (p == NULL) return 0;
    uint32_t old = p->umask;
    p->umask = (uint32_t)new_mask & 0777;
    return (int64_t)old;
}

/* getdents64: records go straight to the user buffer from the directory walk */
typedef struct {
    DirContext ctx;   /* first: the actor gets &ctx */
    uint64_t dirp;
    uint64_t count;
    uint64_t used;
    int err;
} GetdentsCtx;

static int getdents_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    GetdentsCtx *g = (GetdentsCtx *)ctx;
    uint64_t reclen = (DIRENT_NAME_OFF + len + 1 + 7) & ~7ULL;
    if (g->used + reclen > g->count) {
        if (g->used == 0) g->err = -EINVAL;   /* not even one record fits */
        return 1;
    }
    uint8_t rec[(DIRENT_NAME_OFF + VFS_NAME_MAX + 1 + 7) & ~7] __attribute__((aligned(8)));
    struct linux_dirent64 *d = (struct linux_dirent64 *)rec;
    memset(rec, 0, reclen);
    d->d_ino = ino;
    d->d_off = next;
    d->d_reclen = (uint16_t)reclen;
    d->d_type = dtype;
    memcpy(d->d_name, name, len);
    if (copy_to_user((uint8_t *)g->dirp + g->used, rec, reclen) != 0) {
        g->err = -EFAULT;
        return 1;
    }
    g->used += reclen;
    return 0;
}

/* getdents64(fd, dirp, count) — 217. d_off of a record is the position after
 * it (lseek(fd, d_off, SEEK_SET) resumes there). Returns bytes filled, 0 at
 * the end, -EINVAL if not even one record fits. */
static int64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3, (void)a4, (void)a5;
    File *f = get_file((int64_t)fd);
    if (f == NULL) return -EBADF;
    if (count > MAX_RW_COUNT) count = MAX_RW_COUNT;
    GetdentsCtx g = {.ctx = {.actor = getdents_actor}, .dirp = dirp, .count = count};
    /* count 0: no record fits, -EINVAL (as Linux), not -EFAULT */
    int err = count == 0 || user_ptr_valid((void *)dirp, count) ? vfs_iterate(f, &g.ctx) : -EFAULT;
    fput(f);
    if (g.used > 0) return (int64_t)g.used;
    return err != 0 ? err : g.err;
}

void fs_syscalls_init(void) {
    syscall_register(__NR_read, sys_read);
    syscall_register(__NR_write, sys_write);
    syscall_register(__NR_open, sys_open);
    syscall_register(__NR_close, sys_close);
    syscall_register(__NR_stat, sys_stat);
    syscall_register(__NR_fstat, sys_fstat);
    syscall_register(__NR_lstat, sys_lstat);
    syscall_register(__NR_lseek, sys_lseek);
    syscall_register(__NR_ioctl, sys_ioctl);
    syscall_register(__NR_pread64, sys_pread64);
    syscall_register(__NR_pwrite64, sys_pwrite64);
    syscall_register(__NR_readv, sys_readv);
    syscall_register(__NR_writev, sys_writev);
    syscall_register(__NR_pipe, sys_pipe);
    syscall_register(__NR_dup, sys_dup);
    syscall_register(__NR_dup2, sys_dup2);
    syscall_register(__NR_fcntl, sys_fcntl);
    syscall_register(__NR_fsync, sys_fsync);
    syscall_register(__NR_fdatasync, sys_fdatasync);
    syscall_register(__NR_ftruncate, sys_ftruncate);
    syscall_register(__NR_getcwd, sys_getcwd);
    syscall_register(__NR_chdir, sys_chdir);
    syscall_register(__NR_rename, sys_rename);
    syscall_register(__NR_mkdir, sys_mkdir);
    syscall_register(__NR_rmdir, sys_rmdir);
    syscall_register(__NR_unlink, sys_unlink);
    syscall_register(__NR_chmod, sys_chmod);
    syscall_register(__NR_chown, sys_chown);
    syscall_register(__NR_umask, sys_umask);
    syscall_register(__NR_getdents64, sys_getdents64);
    syscall_register(__NR_openat, sys_openat);
    syscall_register(__NR_mkdirat, sys_mkdirat);
    syscall_register(__NR_newfstatat, sys_newfstatat);
    syscall_register(__NR_unlinkat, sys_unlinkat);
    syscall_register(__NR_renameat, sys_renameat);
    syscall_register(__NR_dup3, sys_dup3);
    syscall_register(__NR_pipe2, sys_pipe2);
}
