/* arc_os — file and directory syscalls (Linux x86_64 numbers, ADR-0001).
 *
 * Moved out of arch/x86_64/syscall.c by M0-05. Every handler validates user
 * pointers with user_ptr_valid (range check only; M1-19 adds fault-safe copies)
 * and returns -errno on failure. */

#include "fs/fs_syscalls.h"
#include "fs/vfs.h"
#include "fs/pipe.h"
#include "fs/path.h"
#include "fs/devfs.h"
#include "proc/process.h"
#include "proc/fd.h"
#include "proc/signal.h"
#include "drivers/tty.h"
#include "mm/kmalloc.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "arch/x86_64/syscall.h"
#include "user_access.h"
#include "uapi/unistd.h"
#include "uapi/stat.h"
#include "uapi/dirent.h"
#include "uapi/termios.h"

/* Standard file descriptor numbers: with no fd-table entry they mean the TTY */
#define FD_STDIN   0
#define FD_STDOUT  1
#define FD_STDERR  2

/* getdents64: smallest record (one-character name) and the most entries one
 * directory listing may return. The VFS readdir op lists from the start every
 * time, so a call costs O(offset + batch) entries and a kmalloc of that many
 * VfsDirEntry (272 B each, at most 68 KiB); M1-11 replaces this with a cursor. */
#define DIRENT_NAME_OFF      19
#define DIRENT_MIN_RECLEN    24
#define GETDENTS_MAX_ENTRIES 256

int fs_user_path(uint64_t path_addr, char *abs, size_t size) {
    if (!user_ptr_valid((const void *)path_addr, 1)) return -EFAULT;
    const char *path = (const char *)path_addr;
    Process *p = proc_current();
    const char *cwd = (p != NULL) ? p->cwd : "/";
    return path_normalize(cwd, path, abs, size);
}

/* Close what fd refers to (pipe end refcount) and free the slot. */
static void fd_release(FdTable *ft, int fd) {
    VfsFile *f = fd_get(ft, fd);
    if (f == NULL) return;
    if (f->node && f->node->type == VFS_PIPE) pipe_close(f->node);
    vfs_close(f);
    fd_free(ft, fd);
}

/* read(fd, buf, count) — 0 */
static int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (count == 0) return 0;
    if (!user_ptr_valid((void *)buf_addr, count)) return -EFAULT;

    Process *p = proc_current();
    VfsFile *file = (p != NULL && p->fd_table != NULL) ? fd_get(p->fd_table, (int)fd) : NULL;
    if (file != NULL) return vfs_read(file, (void *)buf_addr, (uint32_t)count);

    /* stdin with no fd-table entry (nothing dup2'd over it) reads the TTY */
    if (fd == FD_STDIN) return tty_read((void *)buf_addr, (uint32_t)count);
    return -EBADF;
}

/* write(fd, buf, count) — 1 */
static int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (count == 0) return 0;
    if (!user_ptr_valid((const void *)buf_addr, count)) return -EFAULT;

    Process *p = proc_current();
    VfsFile *file = (p != NULL && p->fd_table != NULL) ? fd_get(p->fd_table, (int)fd) : NULL;
    if (file != NULL) {
        int64_t ret = vfs_write(file, (const void *)buf_addr, (uint32_t)count);
        if (ret == -EPIPE) sig_send(p->pid, SIGPIPE);
        return ret;
    }

    /* stdout/stderr with no fd-table entry write to the TTY */
    if (fd == FD_STDOUT || fd == FD_STDERR) return tty_write((const void *)buf_addr, (uint32_t)count);
    return -EBADF;
}

/* open(path, flags, mode) — 2. mode is ignored: new files get 0666 & ~umask. */
static int64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)mode; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;

    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;

    int fd = fd_alloc(p->fd_table);
    if (fd < 0) return -EMFILE;
    VfsFile *file = fd_get(p->fd_table, fd);
    int err = vfs_open(abs, (uint32_t)flags, file);
    if (err != 0) {
        fd_free(p->fd_table, fd);
        return err;
    }
    return fd;
}

/* close(fd) — 3 */
static int64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;
    if (fd_get(p->fd_table, (int)fd) == NULL) return -EBADF;
    fd_release(p->fd_table, (int)fd);
    return 0;
}

static uint32_t mode_type_bits(uint8_t vfs_type) {
    switch (vfs_type) {
    case VFS_DIRECTORY: return S_IFDIR;
    case VFS_PIPE:      return S_IFIFO;
    case VFS_SOCKET:    return S_IFSOCK;
    default:            return S_IFREG;
    }
}

/* Fill a Linux struct stat. Times are 0 until the clock work (M1-06). */
static void stat_fill(struct stat *out, uint64_t ino, uint8_t vfs_type, uint64_t size,
                      uint32_t mode, uint32_t uid, uint32_t gid) {
    memset(out, 0, sizeof(*out));
    out->st_ino = ino;
    out->st_nlink = (vfs_type == VFS_DIRECTORY) ? 2 : 1;
    out->st_mode = mode_type_bits(vfs_type) | (mode & 07777);
    out->st_uid = uid;
    out->st_gid = gid;
    out->st_size = (int64_t)size;
    out->st_blksize = 4096;
    out->st_blocks = (int64_t)((size + 511) / 512);
}

/* stat(path, statbuf) — 4 */
static int64_t sys_stat(uint64_t path_addr, uint64_t stat_addr, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_ptr_valid((void *)stat_addr, sizeof(struct stat))) return -EFAULT;

    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;

    VfsStat vs;
    int err = vfs_stat(abs, &vs);
    if (err != 0) return err;
    stat_fill((struct stat *)stat_addr, vs.inode_num, vs.type, vs.size, vs.mode, vs.uid, vs.gid);
    return 0;
}

/* fstat(fd, statbuf) — 5 */
static int64_t sys_fstat(uint64_t fd, uint64_t stat_addr, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_ptr_valid((void *)stat_addr, sizeof(struct stat))) return -EFAULT;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;

    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;

    struct stat *out = (struct stat *)stat_addr;
    if (file->node == NULL) {
        /* Sockets have no VfsNode (the fd stores the socket index) */
        stat_fill(out, 0, VFS_SOCKET, 0, 0777, 0, 0);
        return 0;
    }
    VfsNode *n = file->node;
    stat_fill(out, n->inode_num, n->type, n->size, n->mode, n->uid, n->gid);
    return 0;
}

/* lseek(fd, offset, whence) — 8 */
static int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;

    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;
    if (file->node == NULL || file->node->type == VFS_PIPE) return -ESPIPE;

    return vfs_seek(file, (int64_t)offset, (int)whence);
}

/* ioctl(fd, request, arg) — 16. Only the terminal job-control requests exist
 * so far (TIOCSPGRP/TIOCGPGRP, which replace the old tcsetpgrp syscall). */
static int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -EBADF;

    VfsFile *file = (p->fd_table != NULL) ? fd_get(p->fd_table, (int)fd) : NULL;
    if (file != NULL) {
        if (!devfs_is_tty(file->node)) return -ENOTTY;
    } else if (fd > FD_STDERR) {
        return -EBADF;   /* 0-2 with no entry are the TTY, as in read/write */
    }

    switch (request) {
    case TIOCSPGRP: {
        if (!user_ptr_valid((const void *)arg, sizeof(int32_t))) return -EFAULT;
        int32_t pgid = *(const int32_t *)arg;
        if (pgid <= 0) return -EINVAL;
        tty_set_fg_pgid((uint32_t)pgid);
        return 0;
    }
    case TIOCGPGRP:
        if (!user_ptr_valid((void *)arg, sizeof(int32_t))) return -EFAULT;
        *(int32_t *)arg = (int32_t)tty_get_fg_pgid();
        return 0;
    default:
        return -ENOTTY;
    }
}

/* Initialize a pipe fd entry */
static void pipe_setup_fd(FdTable *ft, int fd, VfsNode *node, uint32_t flags) {
    VfsFile *f = fd_get(ft, fd);
    f->node = node;
    f->offset = 0;
    f->flags = flags;
}

/* pipe(pipefd[2]) — 22 */
static int64_t sys_pipe(uint64_t pipefd_addr, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_ptr_valid((void *)pipefd_addr, 2 * sizeof(int32_t))) return -EFAULT;

    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;

    VfsNode *read_node, *write_node;
    int err = pipe_create(&read_node, &write_node);
    if (err != 0) return err;

    int rfd = fd_alloc(p->fd_table);
    if (rfd < 0) {
        pipe_close(read_node);
        pipe_close(write_node);
        return -EMFILE;
    }
    int wfd = fd_alloc(p->fd_table);
    if (wfd < 0) {
        fd_free(p->fd_table, rfd);
        pipe_close(read_node);
        pipe_close(write_node);
        return -EMFILE;
    }

    pipe_setup_fd(p->fd_table, rfd, read_node, O_RDONLY);
    pipe_setup_fd(p->fd_table, wfd, write_node, O_WRONLY);

    int32_t *user_fds = (int32_t *)pipefd_addr;
    user_fds[0] = (int32_t)rfd;
    user_fds[1] = (int32_t)wfd;
    return 0;
}

/* dup(oldfd) — 32: lowest free fd */
static int64_t sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;

    VfsFile *src = fd_get(p->fd_table, (int)oldfd);
    if (src == NULL) return -EBADF;

    int newfd = fd_alloc(p->fd_table);
    if (newfd < 0) return -EMFILE;

    FdEntry *dst_entry = &p->fd_table->entries[newfd];
    dst_entry->file = *src;
    if (dst_entry->file.node && dst_entry->file.node->type == VFS_PIPE) {
        pipe_addref(dst_entry->file.node);
    }
    return (int64_t)newfd;
}

/* dup2(oldfd, newfd) — 33 */
static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS) return -EBADF;

    VfsFile *src = fd_get(p->fd_table, (int)oldfd);
    if (src == NULL) return -EBADF;
    if (oldfd == newfd) return (int64_t)newfd;

    /* If newfd is open, close it first */
    FdEntry *dst_entry = &p->fd_table->entries[newfd];
    if (dst_entry->in_use) {
        if (dst_entry->file.node && dst_entry->file.node->type == VFS_PIPE) {
            pipe_close(dst_entry->file.node);
        }
        dst_entry->in_use = 0;
    }

    dst_entry->file = *src;
    dst_entry->in_use = 1;
    if (dst_entry->file.node && dst_entry->file.node->type == VFS_PIPE) {
        pipe_addref(dst_entry->file.node);
    }
    return (int64_t)newfd;
}

/* getcwd(buf, size) — 79: returns the length including the NUL */
static int64_t sys_getcwd(uint64_t buf_addr, uint64_t size, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOENT;

    size_t cwd_len = strlen(p->cwd) + 1;
    if (size < cwd_len) return -ERANGE;
    if (!user_ptr_valid((void *)buf_addr, cwd_len)) return -EFAULT;

    memcpy((void *)buf_addr, p->cwd, cwd_len);
    return (int64_t)cwd_len;
}

/* chdir(path) — 80 */
static int64_t sys_chdir(uint64_t path_addr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return -ENOENT;

    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;

    VfsNode *node = vfs_resolve(abs);
    if (node == NULL) return -ENOENT;
    if (node->type != VFS_DIRECTORY) return -ENOTDIR;

    strncpy(p->cwd, abs, PATH_MAX);
    return 0;
}

/* mkdir(path, mode) — 83 */
static int64_t sys_mkdir(uint64_t path_addr, uint64_t mode, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;
    return vfs_mkdir(abs, (uint32_t)mode);
}

/* unlink(path) — 87 */
static int64_t sys_unlink(uint64_t path_addr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;
    return vfs_unlink(abs);
}

/* chmod(path, mode) — 90 */
static int64_t sys_chmod(uint64_t path_addr, uint64_t mode, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;

    VfsNode *node = vfs_resolve(abs);
    if (node == NULL) return -ENOENT;

    Process *p = proc_current();
    if (p == NULL) return -EPERM;
    if (p->euid != 0 && p->euid != node->uid) return -EPERM;

    node->mode = (uint32_t)mode & 07777;
    return 0;
}

/* chown(path, uid, gid) — 92: root only; -1 leaves a field unchanged */
static int64_t sys_chown(uint64_t path_addr, uint64_t new_uid, uint64_t new_gid,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    char abs[PATH_MAX];
    int perr = fs_user_path(path_addr, abs, PATH_MAX);
    if (perr != 0) return perr;

    VfsNode *node = vfs_resolve(abs);
    if (node == NULL) return -ENOENT;

    Process *p = proc_current();
    if (p == NULL || p->euid != 0) return -EPERM;

    if ((uint32_t)new_uid != (uint32_t)-1) node->uid = (uint32_t)new_uid;
    if ((uint32_t)new_gid != (uint32_t)-1) node->gid = (uint32_t)new_gid;
    return 0;
}

/* umask(mask) — 95: returns the previous mask */
static int64_t sys_umask(uint64_t new_mask, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL) return 0;
    uint32_t old = p->umask;
    p->umask = (uint32_t)new_mask & 0777;
    return (int64_t)old;
}

static uint8_t dirent_type(uint8_t vfs_type) {
    switch (vfs_type) {
    case VFS_FILE:      return DT_REG;
    case VFS_DIRECTORY: return DT_DIR;
    case VFS_PIPE:      return DT_FIFO;
    case VFS_SOCKET:    return DT_SOCK;
    default:            return DT_UNKNOWN;
    }
}

/* getdents64(fd, dirp, count) — 217. The fd's offset is the index of the next
 * entry; d_off of each record is the index after it. Returns bytes filled,
 * 0 at the end, -EINVAL if not even one record fits. */
static int64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;
    VfsFile *file = fd_get(p->fd_table, (int)fd);
    if (file == NULL) return -EBADF;
    if (file->node == NULL || file->node->type != VFS_DIRECTORY) return -ENOTDIR;
    if (count == 0) return -EINVAL;
    if (!user_ptr_valid((void *)dirp, count)) return -EFAULT;

    uint64_t start = file->offset;
    if (start >= GETDENTS_MAX_ENTRIES) return 0;
    uint64_t batch = count / DIRENT_MIN_RECLEN + 1;
    uint64_t want = start + batch;
    if (want > GETDENTS_MAX_ENTRIES) want = GETDENTS_MAX_ENTRIES;

    VfsDirEntry *list = kmalloc(want * sizeof(VfsDirEntry), 0);
    if (list == NULL) return -ENOMEM;
    int n = vfs_readdir_node(file->node, list, (uint32_t)want);
    if (n < 0) {
        kfree(list);
        return n;
    }

    uint8_t *out = (uint8_t *)dirp;
    uint64_t pos = 0;
    uint64_t i = start;
    for (; i < (uint64_t)n; i++) {
        list[i].name[VFS_NAME_MAX - 1] = '\0';
        uint64_t namelen = strlen(list[i].name);
        uint64_t reclen = (DIRENT_NAME_OFF + namelen + 1 + 7) & ~7ULL;
        if (pos + reclen > count) break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)(out + pos);
        memset(d, 0, reclen);
        d->d_ino = list[i].inode_num;
        d->d_off = (int64_t)(i + 1);
        d->d_reclen = (uint16_t)reclen;
        d->d_type = dirent_type(list[i].type);
        memcpy(d->d_name, list[i].name, namelen);
        pos += reclen;
    }
    kfree(list);

    if (pos == 0 && i < (uint64_t)n) return -EINVAL;   /* buffer too small */
    file->offset = i;
    return (int64_t)pos;
}

void fs_syscalls_init(void) {
    syscall_register(__NR_read,       sys_read);
    syscall_register(__NR_write,      sys_write);
    syscall_register(__NR_open,       sys_open);
    syscall_register(__NR_close,      sys_close);
    syscall_register(__NR_stat,       sys_stat);
    syscall_register(__NR_fstat,      sys_fstat);
    syscall_register(__NR_lseek,      sys_lseek);
    syscall_register(__NR_ioctl,      sys_ioctl);
    syscall_register(__NR_pipe,       sys_pipe);
    syscall_register(__NR_dup,        sys_dup);
    syscall_register(__NR_dup2,       sys_dup2);
    syscall_register(__NR_getcwd,     sys_getcwd);
    syscall_register(__NR_chdir,      sys_chdir);
    syscall_register(__NR_mkdir,      sys_mkdir);
    syscall_register(__NR_unlink,     sys_unlink);
    syscall_register(__NR_chmod,      sys_chmod);
    syscall_register(__NR_chown,      sys_chown);
    syscall_register(__NR_umask,      sys_umask);
    syscall_register(__NR_getdents64, sys_getdents64);
}
