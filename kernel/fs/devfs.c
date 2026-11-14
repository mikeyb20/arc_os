/* arc_os — devfs: /dev/null, /dev/zero, /dev/tty (devfs.h, M1-11)
 *
 * Owns: the /dev directory and its device nodes. Everything is static: the
 * superblock, the inodes (I_STATIC) and the console File.
 * Invariants: the device list never changes after devfs_init.
 * Locks: none of its own; the tty driver serialises the terminal (under the
 * big kernel lock until lane KB's TTY row comes off it).
 * Cost: lookup scans DEV_COUNT (3) names; reads and writes are O(bytes). */

#include "fs/devfs.h"
#include "drivers/tty.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "user_access.h"
#include "uapi/termios.h"

/* Terminal transfers go through a small kernel buffer: the tty driver
 * takes kernel memory */
#define TTY_CHUNK 512

/* Linux device numbers (major << 8 | minor) */
#define DEVNO(major, minor) (((major) << 8) | (minor))

static SuperBlock devfs_sb;
static Inode dev_root;
static Inode dev_null;
static Inode dev_zero;
static Inode dev_tty;
static File *console;

/* --- /dev/null and /dev/zero --- */

static int64_t null_read(File *file, const IoBuf *buf, loff_t *pos) {
    (void)file, (void)buf, (void)pos;
    return 0;
}

static int64_t sink_write(File *file, const IoBuf *buf, loff_t *pos) {
    (void)file, (void)pos;
    return (int64_t)buf->len;
}

static int64_t zero_read(File *file, const IoBuf *buf, loff_t *pos) {
    (void)file, (void)pos;
    int err = iob_zero(buf, 0, buf->len);
    return err != 0 ? err : (int64_t)buf->len;
}

static loff_t dev_llseek(File *file, loff_t offset, int whence) {
    (void)offset, (void)whence;
    file->pos = 0;   /* Linux: seeking a null device always lands at 0 */
    return 0;
}

static const FileOps null_fops = {.read = null_read, .write = sink_write, .llseek = dev_llseek};
static const FileOps zero_fops = {.read = zero_read, .write = sink_write, .llseek = dev_llseek};

/* --- /dev/tty --- */

static int64_t tty_file_read(File *file, const IoBuf *buf, loff_t *pos) {
    (void)file, (void)pos;
    uint8_t k[TTY_CHUNK];
    size_t n = buf->len < sizeof(k) ? buf->len : sizeof(k);
    int r = tty_read(k, (uint32_t)n);   /* one line at most: a short read is normal */
    if (r <= 0) return r;
    int err = iob_copy_out(buf, 0, k, (size_t)r);
    return err != 0 ? err : r;   /* note: bytes of a line read into a bad buffer are lost */
}

static int64_t tty_file_write(File *file, const IoBuf *buf, loff_t *pos) {
    (void)file, (void)pos;
    uint8_t k[TTY_CHUNK];
    size_t done = 0;
    while (done < buf->len) {
        size_t n = buf->len - done < sizeof(k) ? buf->len - done : sizeof(k);
        if (iob_copy_in(k, buf, done, n) != 0) return done > 0 ? (int64_t)done : -EFAULT;
        int r = tty_write(k, (uint32_t)n);
        if (r < 0) return done > 0 ? (int64_t)done : r;
        done += (size_t)r;
        if ((size_t)r < n) break;
    }
    return (int64_t)done;
}

int64_t devfs_tty_ioctl(File *file, uint64_t request, uint64_t arg) {
    (void)file;
    switch (request) {
    case TIOCSPGRP: {
        int32_t pgid;
        if (get_user(pgid, arg) != 0) return -EFAULT;
        if (pgid <= 0) return -EINVAL;
        tty_set_fg_pgid((uint32_t)pgid);
        return 0;
    }
    case TIOCGPGRP:
        return put_user((int32_t)tty_get_fg_pgid(), arg);
    case TCGETS: {
        struct termios t;
        tty_get_termios(&t);
        return copy_to_user((void *)arg, &t, sizeof(t));
    }
    case TCSETS:
    case TCSETSW: /* output is written synchronously: nothing to drain */
    case TCSETSF: {
        struct termios t;
        if (copy_from_user(&t, (const void *)arg, sizeof(t)) != 0) return -EFAULT;
        if (request == TCSETSF) tty_flush_input();
        tty_set_termios(&t);
        return 0;
    }
    case TIOCGWINSZ: {
        struct winsize ws;
        tty_get_winsize(&ws);
        return copy_to_user((void *)arg, &ws, sizeof(ws));
    }
    case TIOCSWINSZ: {
        struct winsize ws;
        if (copy_from_user(&ws, (const void *)arg, sizeof(ws)) != 0) return -EFAULT;
        tty_set_winsize(&ws);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

static const FileOps tty_fops = {
    .fflags = FOP_STREAM,
    .read = tty_file_read,
    .write = tty_file_write,
    .ioctl = devfs_tty_ioctl,
};

/* --- The /dev directory --- */

static const struct {
    const char *name;
    Inode *inode;
} devices[] = {
    {"null", &dev_null},
    {"zero", &dev_zero},
    {"tty", &dev_tty},
};
#define DEV_COUNT   (sizeof(devices) / sizeof(devices[0]))
#define DEV_COOKIE0 2   /* cookie of devices[0]: after "." and ".." */

static int devfs_lookup(Inode *dir, const char *name, Inode **out) {
    if (strcmp(name, "..") == 0) {   /* the root is the only directory */
        *out = igrab(dir);
        return 0;
    }
    for (uint32_t i = 0; i < DEV_COUNT; i++) {
        if (strcmp(name, devices[i].name) == 0) {
            *out = igrab(devices[i].inode);
            return 0;
        }
    }
    return -ENOENT;
}

static int devfs_iterate(File *file, DirContext *ctx) {
    if (!dir_emit_dots(ctx, file->inode->ino, file->inode->ino)) return 0;
    for (uint64_t i = (uint64_t)ctx->pos - DEV_COOKIE0; i < DEV_COUNT; i++) {
        Inode *d = devices[i].inode;
        if (!dir_emit(ctx, devices[i].name, strlen(devices[i].name), d->ino, vfs_dtype(d->mode),
                      (loff_t)(i + 1 + DEV_COOKIE0)))
            return 0;
    }
    return 0;
}

static const InodeOps devfs_dir_iops = {.lookup = devfs_lookup};
static const FileOps devfs_dir_fops = {.iterate = devfs_iterate};

static void dev_node(Inode *inode, uint64_t ino, uint32_t rdev, const FileOps *fop) {
    inode_init_static(inode, &devfs_sb, ino, S_IFCHR | 0666);
    inode->rdev = rdev;
    inode->fop = fop;
}

SuperBlock *devfs_init(void) {
    vfs_sb_init(&devfs_sb, "devfs", NULL);
    devfs_sb.flags = SB_NO_DCACHE;   /* three static names: the scan is cheaper than caching */
    inode_init_static(&dev_root, &devfs_sb, 1, S_IFDIR | 0755);
    dev_root.nlink = 2;
    dev_root.iop = &devfs_dir_iops;
    dev_root.fop = &devfs_dir_fops;
    dev_node(&dev_null, 2, DEVNO(1, 3), &null_fops);
    dev_node(&dev_zero, 3, DEVNO(1, 5), &zero_fops);
    dev_node(&dev_tty, 4, DEVNO(5, 0), &tty_fops);
    devfs_sb.root = &dev_root;
    console = file_new(igrab(&dev_tty), &tty_fops, O_RDWR);   /* never released */
    return &devfs_sb;
}

File *devfs_console(void) {
    return console != NULL ? fget_ref(console) : NULL;
}
