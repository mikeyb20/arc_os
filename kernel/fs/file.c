/* arc_os — open files (vfs.h, M1-11)
 *
 * Owns: File objects (open, reference counting, release), reads and writes
 * at the shared position or an explicit one, seeking, directory iteration,
 * ftruncate and fsync, and the IoBuf copy helpers.
 *
 * Invariants: a File holds one reference to its inode for its whole life. Its
 * position is read and changed only under pos_lock (seekable files), so two
 * processes sharing it through dup or fork see one offset and each read or
 * write moves it atomically. Stream files (FOP_STREAM: pipes, sockets,
 * terminals) have no position.
 *
 * Locks: pos_lock (LOCK_LEVEL_FILE_POS), then the inode lock for a regular
 * file (shared to read, exclusive to write) or a directory (shared to
 * iterate). Filesystems copy user memory under those sleeping locks; a fault
 * there takes only mm locks, which come later in the order.
 *
 * Cost: O(1) per call above the filesystem; no bounce buffer (data goes
 * straight between user memory and the filesystem through IoBuf). */

#include "fs/vfs_internal.h"
#include "mm/slab.h"
#include "lib/mem.h"
#include "user_access.h"

_Static_assert(sizeof(File) <= 128, "File must stay within two cache lines");

static struct kmem_cache *file_cache;

void vfs_file_init(void) {
    file_cache = kmem_cache_create("file", sizeof(File), 8);
}

/* --- IoBuf --- */

int iob_copy_out(const IoBuf *b, size_t off, const void *src, size_t n) {
    if (n == 0) return 0;
    if (b->user) return copy_to_user(b->base + off, src, n) == 0 ? 0 : -EFAULT;
    memcpy(b->base + off, src, n);
    return 0;
}

int iob_copy_in(void *dst, const IoBuf *b, size_t off, size_t n) {
    if (n == 0) return 0;
    if (b->user) return copy_from_user(dst, b->base + off, n) == 0 ? 0 : -EFAULT;
    memcpy(dst, b->base + off, n);
    return 0;
}

size_t iob_copy_in_partial(void *dst, const IoBuf *b, size_t off, size_t n) {
    if (n == 0) return 0;
    if (b->user) return copy_from_user_partial(dst, b->base + off, n);
    memcpy(dst, b->base + off, n);
    return n;
}

int iob_zero(const IoBuf *b, size_t off, size_t n) {
    static const uint8_t zeros[256];
    if (!b->user) {
        memset(b->base + off, 0, n);
        return 0;
    }
    while (n > 0) {
        size_t c = n < sizeof(zeros) ? n : sizeof(zeros);
        if (copy_to_user(b->base + off, zeros, c) != 0) return -EFAULT;
        off += c;
        n -= c;
    }
    return 0;
}

/* --- File lifetime --- */

File *file_new(Inode *inode, const FileOps *fop, uint32_t flags) {
    File *f = kmem_cache_alloc(file_cache, GFP_ZERO);
    if (f == NULL) return NULL;
    f->inode = inode;
    f->fop = fop;
    f->flags = flags & ~(uint32_t)(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | O_CLOEXEC);
    f->refcount = 1;
    mutex_init(&f->pos_lock, LOCK_LEVEL_FILE_POS);
    return f;
}

File *fget_ref(File *file) {
    __atomic_add_fetch(&file->refcount, 1, __ATOMIC_RELAXED);
    return file;
}

void fput(File *file) {
    if (file == NULL) return;
    if (__atomic_sub_fetch(&file->refcount, 1, __ATOMIC_ACQ_REL) != 0) return;
    if (file->fop != NULL && file->fop->release != NULL) file->fop->release(file->inode, file);
    iput(file->inode);
    kmem_cache_free(file_cache, file);
}

static int is_stream(const File *file) {
    return file->fop == NULL || (file->fop->fflags & FOP_STREAM);
}

/* Permission open() needs for flags on inode. */
static int open_want(uint32_t flags) {
    uint32_t acc = flags & O_ACCMODE;
    int want = 0;
    if (acc == O_RDONLY || acc == O_RDWR) want |= R_OK;
    if (acc == O_WRONLY || acc == O_RDWR) want |= W_OK;
    if (flags & O_TRUNC) want |= W_OK;
    return want;
}

/* Type and permission checks of open() on an existing or new inode. An open
 * for exec needs a regular file with execute permission, not read. */
static int open_check(Inode *inode, uint32_t flags, int created, int exec) {
    if (exec) return S_ISREG(inode->mode) ? inode_permission(inode, X_OK) : -EACCES;
    uint32_t acc = flags & O_ACCMODE;
    if (acc == O_ACCMODE) return -EINVAL;
    if (S_ISDIR(inode->mode)) {
        if (acc != O_RDONLY || (flags & O_TRUNC)) return -EISDIR;
    } else if (flags & O_DIRECTORY) {
        return -ENOTDIR;
    }
    if ((acc != O_RDONLY || (flags & O_TRUNC)) && S_ISREG(inode->mode) && (inode->sb->flags & SB_RDONLY)) return -EROFS;
    if (created) return 0;   /* the creator may open what it made, whatever the mode */
    return inode_permission(inode, open_want(flags));
}

static int open_file(const PathLoc *base, const char *path, uint32_t flags, uint32_t mode, int exec, File **out) {
    PathLoc loc;
    int created;
    int err = vfs_open_locate(base, path, flags, mode, &loc, &created);
    if (err != 0) return err;
    Inode *inode = loc.inode;
    err = open_check(inode, flags, created, exec);
    if (err == 0 && (flags & O_TRUNC) && S_ISREG(inode->mode) && !created) {
        InodeAttr attr = {.valid = ATTR_SIZE, .size = 0};
        err = vfs_setattr(inode, &attr);
    }
    if (err != 0) {
        loc_put(&loc);
        return err;
    }
    File *f = file_new(inode, inode->fop, flags);   /* takes over the inode reference */
    if (f == NULL) {
        loc_put(&loc);
        return -ENOMEM;
    }
    f->mnt = loc.mnt;
    if (f->fop != NULL && f->fop->open != NULL) err = f->fop->open(inode, f);
    if (err != 0) {
        if (f->fop != NULL && f->fop->open != NULL) f->fop = NULL;   /* open failed: no release */
        fput(f);
        return err;
    }
    *out = f;
    return 0;
}

int vfs_open_at(const PathLoc *base, const char *path, uint32_t flags, uint32_t mode, File **out) {
    return open_file(base, path, flags, mode, 0, out);
}

int vfs_open_exec_at(const PathLoc *base, const char *path, File **out) {
    return open_file(base, path, O_RDONLY, 0, 1, out);
}

/* --- Reading and writing --- */

/* Bytes of a write at pos that fit below the size limit, or -EFBIG. */
static int64_t write_limit(loff_t pos, size_t len) {
    if (pos >= VFS_OFFSET_MAX) return -EFBIG;
    uint64_t room = (uint64_t)(VFS_OFFSET_MAX - pos);
    return (int64_t)(len < room ? len : room);
}

/* One read at *pos with the inode lock a regular file needs. */
static int64_t do_read(File *file, const IoBuf *buf, loff_t *pos) {
    Inode *inode = file->inode;
    if (!S_ISREG(inode->mode)) return file->fop->read(file, buf, pos);
    rwsem_down_read(&inode->lock);
    int64_t n = file->fop->read(file, buf, pos);
    rwsem_up_read(&inode->lock);
    return n;
}

/* One write at *pos (the end with append set). */
static int64_t do_write(File *file, const IoBuf *buf, loff_t *pos, int append) {
    Inode *inode = file->inode;
    if (!S_ISREG(inode->mode)) return file->fop->write(file, buf, pos);
    rwsem_down_write(&inode->lock);
    if (append) *pos = inode->size;
    int64_t n = write_limit(*pos, buf->len);
    if (n > 0) {
        IoBuf part = *buf;
        part.len = (size_t)n;
        n = file->fop->write(file, &part, pos);
    }
    if (n > 0) {
        vfs_now(&inode->mtime);
        inode->ctime = inode->mtime;
    }
    rwsem_up_write(&inode->lock);
    return n;
}

static int read_check(const File *file) {
    if ((file->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (S_ISDIR(file->inode->mode)) return -EISDIR;
    if (file->fop == NULL || file->fop->read == NULL) return -EINVAL;
    return 0;
}

static int write_check(const File *file) {
    if ((file->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (file->fop == NULL || file->fop->write == NULL) return -EINVAL;
    return 0;
}

int64_t vfs_read(File *file, const IoBuf *buf) {
    int err = read_check(file);
    if (err != 0) return err;
    if (buf->len == 0) return 0;
    if (is_stream(file)) {
        loff_t none = 0;
        return file->fop->read(file, buf, &none);
    }
    mutex_lock(&file->pos_lock);
    loff_t pos = file->pos;
    int64_t n = do_read(file, buf, &pos);
    if (n > 0) file->pos = pos;
    mutex_unlock(&file->pos_lock);
    return n;
}

int64_t vfs_write(File *file, const IoBuf *buf) {
    int err = write_check(file);
    if (err != 0) return err;
    if (buf->len == 0) return 0;
    if (is_stream(file)) {
        loff_t none = 0;
        return file->fop->write(file, buf, &none);
    }
    mutex_lock(&file->pos_lock);
    loff_t pos = file->pos;
    int64_t n = do_write(file, buf, &pos, (file->flags & O_APPEND) != 0);
    if (n > 0) file->pos = pos;
    mutex_unlock(&file->pos_lock);
    return n;
}

int64_t vfs_pread(File *file, const IoBuf *buf, loff_t pos) {
    int err = read_check(file);
    if (err != 0) return err;
    if (is_stream(file)) return -ESPIPE;
    if (pos < 0) return -EINVAL;
    if (buf->len == 0) return 0;
    return do_read(file, buf, &pos);
}

int64_t vfs_pwrite(File *file, const IoBuf *buf, loff_t pos) {
    int err = write_check(file);
    if (err != 0) return err;
    if (is_stream(file)) return -ESPIPE;
    if (pos < 0) return -EINVAL;
    if (buf->len == 0) return 0;
    /* Linux: a file opened with O_APPEND appends even through pwrite */
    return do_write(file, buf, &pos, (file->flags & O_APPEND) != 0);
}

/* --- Seeking, directories, size --- */

loff_t vfs_generic_llseek(File *file, loff_t offset, int whence) {
    loff_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = file->pos;
        break;
    case SEEK_END:
        if (S_ISDIR(file->inode->mode)) return -EINVAL;
        base = file->inode->size;
        break;
    default:
        return -EINVAL;
    }
    loff_t pos;
    if (__builtin_add_overflow(base, offset, &pos)) return -EOVERFLOW;
    if (pos < 0) return -EINVAL;
    file->pos = pos;
    return pos;
}

loff_t vfs_llseek(File *file, loff_t offset, int whence) {
    if (is_stream(file)) return -ESPIPE;
    mutex_lock(&file->pos_lock);
    loff_t r =
        file->fop->llseek != NULL ? file->fop->llseek(file, offset, whence) : vfs_generic_llseek(file, offset, whence);
    mutex_unlock(&file->pos_lock);
    return r;
}

int vfs_iterate(File *file, DirContext *ctx) {
    Inode *inode = file->inode;
    if (!S_ISDIR(inode->mode) || file->fop == NULL || file->fop->iterate == NULL) return -ENOTDIR;
    mutex_lock(&file->pos_lock);
    ctx->pos = file->pos;
    rwsem_down_read(&inode->lock);
    int err = (inode->flags & I_DEAD) ? 0 : file->fop->iterate(file, ctx);   /* removed: empty */
    rwsem_up_read(&inode->lock);
    file->pos = ctx->pos;
    mutex_unlock(&file->pos_lock);
    return err;
}

int vfs_ftruncate(File *file, loff_t length) {
    if (length < 0) return -EINVAL;
    if ((file->flags & O_ACCMODE) == O_RDONLY) return -EINVAL;
    if (!S_ISREG(file->inode->mode)) return S_ISDIR(file->inode->mode) ? -EISDIR : -EINVAL;
    InodeAttr attr = {.valid = ATTR_SIZE, .size = length};
    return vfs_setattr(file->inode, &attr);
}

int vfs_fsync(File *file) {
    if (file->fop == NULL || file->fop->fsync == NULL) return 0;
    return file->fop->fsync(file);
}

int vfs_write_file(const char *path, const void *data, uint64_t size, uint32_t mode) {
    File *f;
    int err = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, mode, &f);
    if (err != 0) return err;
    uint64_t done = 0;
    while (done < size) {
        IoBuf b = iob_kernel((uint8_t *)data + done, (size_t)(size - done));
        int64_t n = vfs_write(f, &b);
        if (n <= 0) {
            err = n < 0 ? (int)n : -EIO;
            break;
        }
        done += (uint64_t)n;
    }
    fput(f);
    return err;
}
