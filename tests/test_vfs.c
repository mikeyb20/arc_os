/* arc_os — VFS core and ramfs on the real code (M1-11): open flags, 64-bit
 * offsets and sparse files, directories and getdents cookies, unlink while
 * open, rmdir, rename, permissions and the sticky bit, setattr, mounts and
 * bind mounts, the dentry cache, allocation failures and leaks. */

#include "test_framework.h"
#include "vfs_env.h"

#include "fs/vfs_internal.h"
#include "fs/ramfs.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/dirent.h"

#define GIB (1024ll * 1024 * 1024)

/* --- Helpers --- */

static int64_t wr(File *f, const char *s) {
    IoBuf b = iob_kernel((void *)s, strlen(s));
    return vfs_write(f, &b);
}

static int64_t rd(File *f, char *buf, size_t n) {
    IoBuf b = iob_kernel(buf, n);
    int64_t r = vfs_read(f, &b);
    if (r >= 0 && (size_t)r < n) buf[r] = '\0';
    return r;
}

static int mkfile(const char *path, const char *text) {
    return vfs_write_file(path, text, strlen(text), 0644);
}

static loff_t size_of(const char *path) {
    struct stat st;
    return vfs_stat(path, &st) == 0 ? (loff_t)st.st_size : -1;
}

/* getdents collector: names joined by spaces, inos, cookies; stops after max */
typedef struct {
    DirContext ctx;
    char names[512];
    uint64_t inos[64];
    loff_t cookies[64];
    int n;
    int max;
} Listing;

static int list_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    (void)dtype;
    Listing *l = (Listing *)ctx;
    if (l->n >= l->max || l->n >= 64) return 1;
    size_t at = strlen(l->names);
    snprintf(l->names + at, sizeof(l->names) - at, "%s%.*s", at ? " " : "", (int)len, name);
    l->inos[l->n] = ino;
    l->cookies[l->n] = next;
    l->n++;
    return 0;
}

static void listing_init(Listing *l, int max) {
    memset(l, 0, sizeof(*l));
    l->ctx.actor = list_actor;
    l->max = max;
}

/* Every entry of dir, in one pass */
static int list_dir(const char *path, Listing *l) {
    File *f;
    int err = vfs_open(path, O_RDONLY | O_DIRECTORY, 0, &f);
    if (err != 0) return err;
    listing_init(l, 64);
    err = vfs_iterate(f, &l->ctx);
    fput(f);
    return err;
}

/* --- Open, read, write --- */

TEST(root_is_directory) {
    vfs_env_setup();
    struct stat st;
    ASSERT_EQ(vfs_stat("/", &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_EQ(vfs_stat("/tmp", &st), 0);
    ASSERT_EQ(st.st_mode & 07777, 01777);
    return 0;
}

TEST(create_write_read) {
    vfs_env_setup();
    File *f;
    ASSERT_EQ(vfs_open("/tmp/cwr", O_CREAT | O_RDWR, 0644, &f), 0);
    ASSERT_EQ(wr(f, "hello world"), 11);
    ASSERT_EQ(vfs_llseek(f, 0, SEEK_SET), 0);
    char buf[32];
    ASSERT_EQ(rd(f, buf, sizeof(buf)), 11);
    ASSERT_STR_EQ(buf, "hello world");
    ASSERT_EQ(rd(f, buf, sizeof(buf)), 0);   /* EOF */
    fput(f);
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/cwr", &st), 0);
    ASSERT_EQ(st.st_size, 11);
    ASSERT_EQ(st.st_mode, S_IFREG | 0644);
    ASSERT_EQ(st.st_nlink, 1);
    return 0;
}

TEST(open_flags) {
    vfs_env_setup();
    File *f;
    ASSERT_EQ(vfs_open("/tmp/of_missing", O_RDONLY, 0, &f), -ENOENT);
    ASSERT_EQ(mkfile("/tmp/of", "abcdef"), 0);
    ASSERT_EQ(vfs_open("/tmp/of", O_CREAT | O_EXCL | O_WRONLY, 0644, &f), -EEXIST);
    ASSERT_EQ(vfs_open("/tmp/of", O_RDONLY | O_DIRECTORY, 0, &f), -ENOTDIR);
    ASSERT_EQ(vfs_open("/tmp/of", O_ACCMODE, 0, &f), -EINVAL);
    ASSERT_EQ(vfs_open("/tmp", O_WRONLY, 0, &f), -EISDIR);
    ASSERT_EQ(vfs_open("/tmp/of/x", O_RDONLY, 0, &f), -ENOTDIR);
    ASSERT_EQ(vfs_open("/tmp", O_CREAT | O_RDWR, 0644, &f), -EISDIR);
    /* O_TRUNC empties; O_EXCL on a missing name creates */
    ASSERT_EQ(vfs_open("/tmp/of", O_WRONLY | O_TRUNC, 0, &f), 0);
    fput(f);
    ASSERT_EQ(size_of("/tmp/of"), 0);
    ASSERT_EQ(vfs_open("/tmp/of_new", O_CREAT | O_EXCL | O_WRONLY, 0600, &f), 0);
    fput(f);
    ASSERT_EQ(size_of("/tmp/of_new"), 0);
    return 0;
}

TEST(access_mode_enforced) {
    vfs_env_setup();
    ASSERT_EQ(mkfile("/tmp/am", "data"), 0);
    File *f;
    char buf[8];
    ASSERT_EQ(vfs_open("/tmp/am", O_RDONLY, 0, &f), 0);
    ASSERT_EQ(wr(f, "x"), -EBADF);
    ASSERT_EQ(vfs_ftruncate(f, 0), -EINVAL);
    fput(f);
    ASSERT_EQ(vfs_open("/tmp/am", O_WRONLY, 0, &f), 0);
    ASSERT_EQ(rd(f, buf, sizeof(buf)), -EBADF);
    fput(f);
    ASSERT_EQ(vfs_open("/tmp", O_RDONLY, 0, &f), 0);
    ASSERT_EQ(rd(f, buf, sizeof(buf)), -EISDIR);
    fput(f);
    return 0;
}

TEST(append_writes_at_end) {
    vfs_env_setup();
    ASSERT_EQ(mkfile("/tmp/ap", "abc"), 0);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/ap", O_WRONLY | O_APPEND, 0, &f), 0);
    ASSERT_EQ(vfs_llseek(f, 0, SEEK_SET), 0);
    ASSERT_EQ(wr(f, "de"), 2);
    ASSERT_EQ(f->pos, 5);
    IoBuf b = iob_kernel("f", 1);
    ASSERT_EQ(vfs_pwrite(f, &b, 0), 1);   /* Linux: O_APPEND wins over pwrite's offset */
    fput(f);
    char buf[16];
    ASSERT_EQ(vfs_env_read_file("/tmp/ap", buf, sizeof(buf)), 6);
    ASSERT_STR_EQ(buf, "abcdef");
    return 0;
}

TEST(sparse_file_beyond_4gib) {
    vfs_env_setup();
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/sparse", O_CREAT | O_RDWR, 0644, &f), 0);
    loff_t at = 5 * GIB + 123;
    IoBuf b = iob_kernel("xyz", 3);
    ASSERT_EQ(vfs_pwrite(f, &b, at), 3);
    ASSERT_EQ(f->inode->size, at + 3);
    vfs_env_usage(&after);
    ASSERT_EQ(after.pages - before.pages, 1);   /* only the page written holds memory */

    char buf[8] = {0};
    IoBuf r = iob_kernel(buf, 3);
    ASSERT_EQ(vfs_pread(f, &r, at), 3);
    ASSERT_MEM_EQ(buf, "xyz", 3);
    memset(buf, 'q', sizeof(buf));
    r = iob_kernel(buf, 8);
    ASSERT_EQ(vfs_pread(f, &r, 4 * GIB + 7), 8);   /* a hole reads as zeros */
    for (int i = 0; i < 8; i++) ASSERT_EQ(buf[i], 0);

    /* lseek past 4 GiB, then read through the file position */
    ASSERT_EQ(vfs_llseek(f, at, SEEK_SET), at);
    ASSERT_EQ(rd(f, buf, 3), 3);
    ASSERT_MEM_EQ(buf, "xyz", 3);
    ASSERT_EQ(vfs_llseek(f, 0, SEEK_CUR), at + 3);
    ASSERT_EQ(vfs_llseek(f, -3, SEEK_END), at);
    struct stat st;
    vfs_fill_stat(f->inode, &st);
    ASSERT_EQ(st.st_size, at + 3);
    ASSERT_EQ(st.st_blocks, 8);   /* one 4 KiB page */
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/sparse"), 0);
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    return 0;
}

TEST(seek_and_offset_limits) {
    vfs_env_setup();
    ASSERT_EQ(mkfile("/tmp/sk", "0123456789"), 0);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/sk", O_RDWR, 0, &f), 0);
    ASSERT_EQ(vfs_llseek(f, -1, SEEK_SET), -EINVAL);
    ASSERT_EQ(vfs_llseek(f, 4, 99), -EINVAL);
    ASSERT_EQ(vfs_llseek(f, VFS_OFFSET_MAX, SEEK_END), -EOVERFLOW);
    ASSERT_EQ(vfs_llseek(f, 20, SEEK_SET), 20);   /* past the end is fine */
    char buf[4];
    ASSERT_EQ(rd(f, buf, sizeof(buf)), 0);
    IoBuf b = iob_kernel("z", 1);
    ASSERT_EQ(vfs_pwrite(f, &b, -1), -EINVAL);
    ASSERT_EQ(vfs_pwrite(f, &b, VFS_OFFSET_MAX), -EFBIG);
    ASSERT_EQ(vfs_pread(f, &b, -5), -EINVAL);
    fput(f);
    ASSERT_EQ(vfs_open("/tmp", O_RDONLY, 0, &f), 0);
    ASSERT_EQ(vfs_llseek(f, 0, SEEK_END), -EINVAL);
    fput(f);
    return 0;
}

TEST(shared_offset_between_references) {
    vfs_env_setup();
    ASSERT_EQ(mkfile("/tmp/sh", "abcdef"), 0);
    File *a;
    ASSERT_EQ(vfs_open("/tmp/sh", O_RDONLY, 0, &a), 0);
    File *b = fget_ref(a);   /* what dup and fork hand the new fd */
    char buf[4];
    ASSERT_EQ(rd(a, buf, 2), 2);
    ASSERT_EQ(rd(b, buf, 2), 2);
    ASSERT_MEM_EQ(buf, "cd", 2);
    fput(b);
    ASSERT_EQ(rd(a, buf, 2), 2);
    ASSERT_MEM_EQ(buf, "ef", 2);
    fput(a);
    /* A second open has its own offset */
    File *c, *d;
    ASSERT_EQ(vfs_open("/tmp/sh", O_RDONLY, 0, &c), 0);
    ASSERT_EQ(vfs_open("/tmp/sh", O_RDONLY, 0, &d), 0);
    ASSERT_EQ(rd(c, buf, 3), 3);
    ASSERT_EQ(rd(d, buf, 1), 1);
    ASSERT_EQ(buf[0], 'a');
    fput(c);
    fput(d);
    return 0;
}

TEST(bad_user_buffer_faults) {
    vfs_env_setup();
    ASSERT_EQ(mkfile("/tmp/fault", "abc"), 0);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/fault", O_RDWR, 0, &f), 0);
    IoBuf bad = iob_user(VFS_ENV_BAD_UADDR, 3);
    ASSERT_EQ(vfs_read(f, &bad), -EFAULT);
    ASSERT_EQ(f->pos, 0);
    ASSERT_EQ(vfs_write(f, &bad), -EFAULT);
    ASSERT_EQ(f->inode->size, 3);
    fput(f);
    return 0;
}

TEST(write_updates_times) {
    vfs_env_setup();
    vfs_env_uptime_ms = 5000;
    ASSERT_EQ(mkfile("/tmp/tm", "a"), 0);
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/tm", &st), 0);
    ASSERT_EQ(st.st_mtime, 5);
    vfs_env_uptime_ms = 9000;
    File *f;
    ASSERT_EQ(vfs_open("/tmp/tm", O_WRONLY, 0, &f), 0);
    ASSERT_EQ(wr(f, "b"), 1);
    fput(f);
    ASSERT_EQ(vfs_stat("/tmp/tm", &st), 0);
    ASSERT_EQ(st.st_mtime, 9);
    ASSERT_EQ(st.st_ctime, 9);
    return 0;
}

/* --- Truncate and setattr --- */

TEST(truncate_shrinks_and_zero_fills) {
    vfs_env_setup();
    ASSERT_EQ(mkfile("/tmp/tr", "0123456789"), 0);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/tr", O_RDWR, 0, &f), 0);
    ASSERT_EQ(vfs_ftruncate(f, 4), 0);
    ASSERT_EQ(f->inode->size, 4);
    ASSERT_EQ(vfs_ftruncate(f, 8192), 0);   /* grows sparse */
    char buf[16];
    IoBuf b = iob_kernel(buf, 10);
    ASSERT_EQ(vfs_pread(f, &b, 0), 10);
    ASSERT_MEM_EQ(buf, "0123\0\0\0\0\0\0", 10);   /* old bytes past the cut stay gone */
    ASSERT_EQ(vfs_ftruncate(f, -1), -EINVAL);
    fput(f);
    return 0;
}

TEST(setattr_rules) {
    vfs_env_setup();
    ASSERT_EQ(mkfile("/tmp/sa", "x"), 0);
    PathLoc loc;
    ASSERT_EQ(vfs_resolve("/tmp/sa", &loc), 0);
    InodeAttr chmod = {.valid = ATTR_MODE, .mode = 0600};
    InodeAttr chown = {.valid = ATTR_UID, .uid = 1000};
    vfs_env_as(1000, 1000);
    ASSERT_EQ(vfs_setattr(loc.inode, &chmod), -EPERM);   /* not the owner */
    vfs_env_as(0, 0);
    ASSERT_EQ(vfs_setattr(loc.inode, &chown), 0);
    vfs_env_as(1000, 1000);
    ASSERT_EQ(vfs_setattr(loc.inode, &chmod), 0);         /* the owner now */
    ASSERT_EQ(loc.inode->mode, S_IFREG | 0600);
    InodeAttr giveaway = {.valid = ATTR_UID, .uid = 0};
    ASSERT_EQ(vfs_setattr(loc.inode, &giveaway), -EPERM);  /* only root chowns */
    loc_put(&loc);
    vfs_env_as(0, 0);
    ASSERT_EQ(vfs_resolve("/tmp", &loc), 0);
    InodeAttr sz = {.valid = ATTR_SIZE, .size = 0};
    ASSERT_EQ(vfs_setattr(loc.inode, &sz), -EISDIR);
    loc_put(&loc);
    return 0;
}

/* --- Directories --- */

TEST(mkdir_and_nlink) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/md", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/md", 0755), -EEXIST);
    ASSERT_EQ(vfs_mkdir("/tmp/nope/sub", 0755), -ENOENT);
    ASSERT_EQ(vfs_mkdir("/", 0755), -EEXIST);
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/md", &st), 0);
    ASSERT_EQ(st.st_nlink, 2);
    ASSERT_EQ(vfs_mkdir("/tmp/md/a", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/md/b", 0755), 0);
    ASSERT_EQ(vfs_stat("/tmp/md", &st), 0);
    ASSERT_EQ(st.st_nlink, 4);   /* ".", its entry, two ".." */
    ASSERT_EQ(vfs_rmdir("/tmp/md/a"), 0);
    ASSERT_EQ(vfs_stat("/tmp/md", &st), 0);
    ASSERT_EQ(st.st_nlink, 3);
    return 0;
}

TEST(getdents_lists_dots_then_entries) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/gd", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/gd/one", "1"), 0);
    ASSERT_EQ(mkfile("/tmp/gd/two", "2"), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/gd/three", 0755), 0);
    Listing l;
    ASSERT_EQ(list_dir("/tmp/gd", &l), 0);
    ASSERT_STR_EQ(l.names, ". .. one two three");
    struct stat self, parent;
    ASSERT_EQ(vfs_stat("/tmp/gd", &self), 0);
    ASSERT_EQ(vfs_stat("/tmp", &parent), 0);
    ASSERT_EQ(l.inos[0], self.st_ino);
    ASSERT_EQ(l.inos[1], parent.st_ino);
    for (int i = 1; i < l.n; i++) ASSERT_TRUE(l.cookies[i] > l.cookies[i - 1]);
    return 0;
}

TEST(getdents_resumes_by_cookie) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/gr", 0755), 0);
    const char *names[] = {"a", "b", "c", "d"};
    for (int i = 0; i < 4; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/tmp/gr/%s", names[i]);
        ASSERT_EQ(mkfile(p, "x"), 0);
    }
    File *f;
    ASSERT_EQ(vfs_open("/tmp/gr", O_RDONLY, 0, &f), 0);
    Listing l;
    listing_init(&l, 3);   /* ". .. a", then the buffer is full */
    ASSERT_EQ(vfs_iterate(f, &l.ctx), 0);
    ASSERT_STR_EQ(l.names, ". .. a");
    ASSERT_EQ(vfs_unlink("/tmp/gr/b"), 0);   /* removed between calls: never listed */
    listing_init(&l, 64);
    ASSERT_EQ(vfs_iterate(f, &l.ctx), 0);
    ASSERT_STR_EQ(l.names, "c d");
    listing_init(&l, 64);
    ASSERT_EQ(vfs_iterate(f, &l.ctx), 0);   /* at the end */
    ASSERT_EQ(l.n, 0);
    ASSERT_EQ(vfs_llseek(f, 0, SEEK_SET), 0);   /* rewinddir */
    ASSERT_EQ(vfs_iterate(f, &l.ctx), 0);
    ASSERT_STR_EQ(l.names, ". .. a c d");
    fput(f);
    return 0;
}

TEST(unlink_rules) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/ul", 0755), 0);
    ASSERT_EQ(vfs_unlink("/tmp/ul"), -EISDIR);
    ASSERT_EQ(vfs_unlink("/tmp/ul/none"), -ENOENT);
    ASSERT_EQ(mkfile("/tmp/ul/f", "x"), 0);
    ASSERT_EQ(vfs_unlink("/tmp/ul/f"), 0);
    ASSERT_EQ(vfs_unlink("/tmp/ul/f"), -ENOENT);
    ASSERT_EQ(size_of("/tmp/ul/f"), -1);
    return 0;
}

TEST(unlinked_file_stays_readable_while_open) {
    vfs_env_setup();
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    ASSERT_EQ(mkfile("/tmp/uo", "still here"), 0);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/uo", O_RDWR, 0, &f), 0);
    ASSERT_EQ(vfs_unlink("/tmp/uo"), 0);
    ASSERT_EQ(f->inode->nlink, 0);
    char buf[32];
    ASSERT_EQ(rd(f, buf, sizeof(buf)), 10);
    ASSERT_STR_EQ(buf, "still here");
    ASSERT_EQ(wr(f, "!"), 1);
    ASSERT_EQ(mkfile("/tmp/uo", "new"), 0);   /* the name is free again */
    ASSERT_EQ(size_of("/tmp/uo"), 3);
    ASSERT_EQ(vfs_unlink("/tmp/uo"), 0);
    fput(f);   /* the last reference frees the old inode and its pages */
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    return 0;
}

TEST(rmdir_rules) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/rd", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/rd/f", "x"), 0);
    ASSERT_EQ(vfs_rmdir("/tmp/rd"), -ENOTEMPTY);
    ASSERT_EQ(vfs_rmdir("/tmp/rd/f"), -ENOTDIR);
    ASSERT_EQ(vfs_rmdir("/"), -EBUSY);
    ASSERT_EQ(vfs_rmdir("/dev"), -EBUSY);   /* a mount point */
    ASSERT_EQ(vfs_unlink("/tmp/rd/f"), 0);
    /* An open, removed directory lists nothing and takes no new entries */
    File *f;
    ASSERT_EQ(vfs_open("/tmp/rd", O_RDONLY, 0, &f), 0);
    ASSERT_EQ(vfs_rmdir("/tmp/rd"), 0);
    Listing l;
    listing_init(&l, 64);
    ASSERT_EQ(vfs_iterate(f, &l.ctx), 0);
    ASSERT_EQ(l.n, 0);
    ASSERT_EQ(f->inode->flags & I_DEAD, I_DEAD);
    fput(f);
    ASSERT_EQ(mkfile("/tmp/rd/g", "x"), -ENOENT);
    return 0;
}

TEST(name_limits) {
    vfs_env_setup();
    char path[300];
    memset(path, 0, sizeof(path));
    strcpy(path, "/tmp/");
    memset(path + 5, 'n', 255);
    ASSERT_EQ(mkfile(path, "x"), 0);   /* 255 bytes: the limit */
    ASSERT_EQ(size_of(path), 1);
    ASSERT_EQ(vfs_unlink(path), 0);
    path[5 + 255] = 'n';
    ASSERT_EQ(mkfile(path, "x"), -ENAMETOOLONG);
    return 0;
}

/* --- Rename --- */

TEST(rename_within_and_across_directories) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/rn", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/rn/a", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/rn/b", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/rn/a/f", "data"), 0);
    struct stat before, after;
    ASSERT_EQ(vfs_stat("/tmp/rn/a/f", &before), 0);
    ASSERT_EQ(vfs_rename("/tmp/rn/a/f", "/tmp/rn/a/g"), 0);
    ASSERT_EQ(size_of("/tmp/rn/a/f"), -1);
    ASSERT_EQ(vfs_rename("/tmp/rn/a/g", "/tmp/rn/b/h"), 0);
    ASSERT_EQ(vfs_stat("/tmp/rn/b/h", &after), 0);
    ASSERT_EQ(after.st_ino, before.st_ino);   /* same inode, new name */
    char buf[8];
    ASSERT_EQ(vfs_env_read_file("/tmp/rn/b/h", buf, sizeof(buf)), 4);
    ASSERT_EQ(vfs_rename("/tmp/rn/b/h", "/tmp/rn/b/h"), 0);   /* to itself: nothing */
    ASSERT_EQ(vfs_rename("/tmp/rn/none", "/tmp/rn/x"), -ENOENT);
    return 0;
}

TEST(rename_replaces_and_type_checks) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/rt", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/rt/old", "new text"), 0);
    ASSERT_EQ(mkfile("/tmp/rt/victim", "old text"), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/rt/d", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/rt/full", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/rt/full/x", "x"), 0);
    ASSERT_EQ(vfs_rename("/tmp/rt/old", "/tmp/rt/d"), -EISDIR);
    ASSERT_EQ(vfs_rename("/tmp/rt/d", "/tmp/rt/victim"), -ENOTDIR);
    ASSERT_EQ(vfs_rename("/tmp/rt/d", "/tmp/rt/full"), -ENOTEMPTY);
    ASSERT_EQ(vfs_rename("/tmp/rt/d", "/tmp/rt/d/sub"), -EINVAL);    /* into itself */
    ASSERT_EQ(vfs_rename("/tmp/rt/full/x", "/tmp/rt/full"), -ENOTEMPTY);   /* Linux: onto an ancestor */
    ASSERT_EQ(vfs_rename("/tmp/rt/full", "/tmp"), -ENOTEMPTY);       /* onto an ancestor */
    ASSERT_EQ(vfs_rename("/tmp/rt/old", "/dev/old"), -EXDEV);
    ASSERT_EQ(vfs_rename("/tmp/rt/old", "/tmp/rt/victim"), 0);
    char buf[16];
    ASSERT_EQ(vfs_env_read_file("/tmp/rt/victim", buf, sizeof(buf)), 8);
    ASSERT_STR_EQ(buf, "new text");
    ASSERT_EQ(vfs_mkdir("/tmp/rt/empty", 0755), 0);
    ASSERT_EQ(vfs_rename("/tmp/rt/d", "/tmp/rt/empty"), 0);   /* an empty directory may be replaced */
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/rt/d", &st), -ENOENT);
    ASSERT_EQ(vfs_stat("/tmp/rt/empty", &st), 0);
    return 0;
}

TEST(rename_directory_moves_dotdot) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/rdd", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/rdd/p1", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/rdd/p2", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/rdd/p1/kid", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/rdd/p1/kid/f", "x"), 0);
    ASSERT_EQ(vfs_rename("/tmp/rdd/p1/kid", "/tmp/rdd/p2/kid"), 0);
    struct stat p1, p2;
    ASSERT_EQ(vfs_stat("/tmp/rdd/p1", &p1), 0);
    ASSERT_EQ(vfs_stat("/tmp/rdd/p2", &p2), 0);
    ASSERT_EQ(p1.st_nlink, 2);
    ASSERT_EQ(p2.st_nlink, 3);
    Listing l;
    ASSERT_EQ(list_dir("/tmp/rdd/p2/kid", &l), 0);
    ASSERT_STR_EQ(l.names, ". .. f");
    ASSERT_EQ(l.inos[1], p2.st_ino);   /* ".." is the new parent */
    ASSERT_EQ(size_of("/tmp/rdd/p2/kid/f"), 1);
    return 0;
}

/* M1-13: the walk starts from an inode, not path text: "..", "." and
 * repeated slashes are walked; a base directory keeps working after it is
 * renamed; its path (getcwd) follows the rename; rename's ancestry checks
 * walk ".." through the tree */
TEST(walk_from_a_directory_and_rename_ancestry) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/wk", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/wk/a", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/wk/a/b", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/wk/a/b/c", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/wk/a/b/c/f", "hi"), 0);
    struct stat st, st2;
    ASSERT_EQ(vfs_stat("/../..//tmp/./wk/a/b/../b/c//f", &st), 0);   /* ".." at "/" stays */
    ASSERT_EQ(st.st_size, 2);
    ASSERT_EQ(vfs_stat("/tmp/wk/a/b/c/f/", &st), -ENOTDIR);   /* trailing '/' wants a directory */
    ASSERT_EQ(vfs_stat("/tmp/wk/a/b/c/f/..", &st), -ENOTDIR);
    ASSERT_EQ(vfs_stat("/tmp/wk/a/b/c/", &st), 0);
    ASSERT_EQ(vfs_stat("", &st), -ENOENT);

    PathLoc base;
    ASSERT_EQ(vfs_resolve("/tmp/wk/a/b", &base), 0);
    ASSERT_EQ(vfs_stat_at(&base, "c/f", WALK_FOLLOW, &st), 0);
    ASSERT_EQ(vfs_stat_at(&base, "..", WALK_FOLLOW, &st), 0);
    ASSERT_EQ(vfs_stat("/tmp/wk/a", &st2), 0);
    ASSERT_EQ(st.st_ino, st2.st_ino);
    char path[64];
    ASSERT_EQ(vfs_path_of(&base, path, sizeof(path)), 11);
    ASSERT_STR_EQ(path, "/tmp/wk/a/b");
    ASSERT_EQ(vfs_path_of(&base, path, 11), -ERANGE);   /* no room for the NUL */

    ASSERT_EQ(vfs_rename("/tmp/wk/a", "/tmp/wk/z"), 0);
    ASSERT_EQ(vfs_stat_at(&base, "c/f", WALK_FOLLOW, &st), 0);   /* the base moved with it */
    ASSERT_EQ(vfs_path_of(&base, path, sizeof(path)), 11);
    ASSERT_STR_EQ(path, "/tmp/wk/z/b");
    ASSERT_EQ(vfs_mkdir_at(&base, "new", 0755), 0);
    ASSERT_EQ(vfs_stat("/tmp/wk/z/b/new", &st), 0);

    /* Ancestry: a directory into its own subtree is -EINVAL at any depth; over
     * one of its ancestors -ENOTEMPTY (that ancestor holds it) */
    ASSERT_EQ(vfs_rename("/tmp/wk/z", "/tmp/wk/z/b/c/x"), -EINVAL);
    ASSERT_EQ(vfs_rename("/tmp/wk/z/b", "/tmp/wk/z/b/new/x"), -EINVAL);
    ASSERT_EQ(vfs_rename_at(&base, "c", NULL, "/tmp/wk/z"), -ENOTEMPTY);
    ASSERT_EQ(vfs_rename("/tmp/wk/z/b/c", "/tmp/wk/c2"), 0);   /* up two levels is fine */
    ASSERT_EQ(vfs_rename("/tmp/wk/c2", "/tmp/wk/z/b/new/c3"), 0);   /* and down */
    ASSERT_EQ(vfs_stat_at(&base, "new/c3/f", WALK_FOLLOW, &st), 0);

    /* Dot names */
    ASSERT_EQ(vfs_mkdir_at(&base, ".", 0755), -EEXIST);
    ASSERT_EQ(vfs_mkdir_at(&base, "..", 0755), -EEXIST);
    ASSERT_EQ(vfs_rmdir_at(&base, "."), -EINVAL);
    ASSERT_EQ(vfs_rmdir_at(&base, ".."), -ENOTEMPTY);
    ASSERT_EQ(vfs_unlink_at(&base, ".."), -EISDIR);
    ASSERT_EQ(vfs_rename_at(&base, ".", NULL, "/tmp/wk/q"), -EBUSY);
    ASSERT_EQ(vfs_unlink_at(&base, "new/c3/f/"), -ENOTDIR);

    /* A removed directory: walks from it find nothing, it has no path */
    ASSERT_EQ(vfs_unlink("/tmp/wk/z/b/new/c3/f"), 0);
    ASSERT_EQ(vfs_rmdir("/tmp/wk/z/b/new/c3"), 0);
    ASSERT_EQ(vfs_rmdir("/tmp/wk/z/b/new"), 0);
    ASSERT_EQ(vfs_rmdir("/tmp/wk/z/b"), 0);
    ASSERT_EQ(vfs_path_of(&base, path, sizeof(path)), -ENOENT);
    ASSERT_EQ(vfs_stat_at(&base, ".", WALK_FOLLOW, &st), 0);   /* itself still stats */
    ASSERT_EQ(vfs_mkdir_at(&base, "x", 0755), -ENOENT);
    File *f;
    ASSERT_EQ(vfs_open_at(&base, "y", O_CREAT | O_WRONLY, 0644, &f), -ENOENT);
    loc_put(&base);
    return 0;
}

/* --- Permissions --- */

TEST(permissions_on_create_and_open) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/pm", 0755), 0);   /* root's, not writable by others */
    ASSERT_EQ(mkfile("/tmp/pm/secret", "s"), 0);
    PathLoc loc;
    ASSERT_EQ(vfs_resolve("/tmp/pm/secret", &loc), 0);
    InodeAttr a = {.valid = ATTR_MODE, .mode = 0600};
    ASSERT_EQ(vfs_setattr(loc.inode, &a), 0);
    loc_put(&loc);
    vfs_env_as(1000, 1000);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/pm/secret", O_RDONLY, 0, &f), -EACCES);
    ASSERT_EQ(vfs_open("/tmp/pm/mine", O_CREAT | O_WRONLY, 0644, &f), -EACCES);
    ASSERT_EQ(vfs_mkdir("/tmp/pm/d", 0755), -EACCES);
    ASSERT_EQ(vfs_unlink("/tmp/pm/secret"), -EACCES);
    /* /tmp is 01777: anyone may create; the file is the creator's */
    ASSERT_EQ(vfs_open("/tmp/u1000", O_CREAT | O_WRONLY, 0666, &f), 0);
    ASSERT_EQ(f->inode->uid, 1000);
    ASSERT_EQ(f->inode->gid, 1000);
    ASSERT_EQ(f->inode->mode, S_IFREG | 0644);   /* umask 022 */
    fput(f);
    vfs_env_as(0, 0);
    return 0;
}

TEST(search_permission_on_path) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/sp", 0700), 0);
    ASSERT_EQ(mkfile("/tmp/sp/f", "x"), 0);
    vfs_env_as(1000, 1000);
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/sp/f", &st), -EACCES);
    vfs_env_as(0, 0);
    return 0;
}

TEST(sticky_directory_protects_entries) {
    vfs_env_setup();
    vfs_env_as(1000, 1000);
    ASSERT_EQ(mkfile("/tmp/st_1000", "x"), 0);
    vfs_env_as(1001, 1001);
    ASSERT_EQ(vfs_unlink("/tmp/st_1000"), -EPERM);
    ASSERT_EQ(vfs_rename("/tmp/st_1000", "/tmp/st_mine"), -EPERM);
    vfs_env_as(1000, 1000);
    ASSERT_EQ(vfs_unlink("/tmp/st_1000"), 0);
    vfs_env_as(0, 0);
    return 0;
}

TEST(umask_applies) {
    vfs_env_setup();
    vfs_env_proc.umask = 077;
    File *f;
    ASSERT_EQ(vfs_open("/tmp/um", O_CREAT | O_WRONLY, 0666, &f), 0);
    ASSERT_EQ(f->inode->mode, S_IFREG | 0600);
    fput(f);
    ASSERT_EQ(vfs_mkdir("/tmp/umd", 0777), 0);
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/umd", &st), 0);
    ASSERT_EQ(st.st_mode, S_IFDIR | 0700);
    vfs_env_proc.umask = 022;
    return 0;
}

/* --- Mounts --- */

TEST(mounts_and_proc_mounts) {
    vfs_env_setup();
    struct stat tmp, dev;
    ASSERT_EQ(vfs_stat("/tmp", &tmp), 0);
    ASSERT_EQ(vfs_stat("/dev", &dev), 0);
    ASSERT_NEQ(tmp.st_dev, dev.st_dev);   /* another filesystem */
    char buf[1024];
    vfs_format_mounts(buf, sizeof(buf));
    ASSERT_TRUE(strstr(buf, "ramfs / ramfs rw 0 0\n") != NULL);
    ASSERT_TRUE(strstr(buf, "devfs /dev devfs rw 0 0\n") != NULL);
    ASSERT_TRUE(strstr(buf, "proc /proc proc rw 0 0\n") != NULL);
    ASSERT_EQ(mkfile("/tmp/mfile", "x"), 0);
    ASSERT_EQ(vfs_mount("/tmp/mfile", ramfs_create_sb(), "ramfs"), -ENOTDIR);
    ASSERT_EQ(vfs_mount("/tmp/nodir", ramfs_create_sb(), "ramfs"), -ENOENT);
    ASSERT_EQ(vfs_mount("/dev", ramfs_create_sb(), "ramfs"), -EBUSY);
    return 0;
}

TEST(mount_covers_directory) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/mnt", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/mnt/hidden", "x"), 0);
    ASSERT_EQ(vfs_mount("/tmp/mnt", ramfs_create_sb(), "scratch"), 0);
    ASSERT_EQ(size_of("/tmp/mnt/hidden"), -1);   /* covered by the new root */
    ASSERT_EQ(mkfile("/tmp/mnt/inner", "abc"), 0);
    ASSERT_EQ(size_of("/tmp/mnt/inner"), 3);
    ASSERT_EQ(vfs_rename("/tmp/mnt/inner", "/tmp/outer"), -EXDEV);
    ASSERT_EQ(vfs_rmdir("/tmp/mnt"), -EBUSY);
    return 0;
}

TEST(bind_mount_shares_subtree) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/data", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/data/apps", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/data/apps/tool", "bin"), 0);
    ASSERT_EQ(vfs_mkdir("/tmp/apps", 0755), 0);
    ASSERT_EQ(vfs_bind_mount("/tmp/data/apps", "/tmp/apps"), 0);
    char buf[8];
    ASSERT_EQ(vfs_env_read_file("/tmp/apps/tool", buf, sizeof(buf)), 3);
    ASSERT_EQ(mkfile("/tmp/apps/new", "made via bind"), 0);
    ASSERT_EQ(size_of("/tmp/data/apps/new"), 13);   /* one tree, two paths */
    struct stat a, b;
    ASSERT_EQ(vfs_stat("/tmp/apps/tool", &a), 0);
    ASSERT_EQ(vfs_stat("/tmp/data/apps/tool", &b), 0);
    ASSERT_EQ(a.st_ino, b.st_ino);
    ASSERT_EQ(a.st_dev, b.st_dev);
    /* Different mounts of one filesystem: rename between them is EXDEV, as on Linux */
    ASSERT_EQ(vfs_rename("/tmp/apps/new", "/tmp/data/new"), -EXDEV);
    char m[1024];
    vfs_format_mounts(m, sizeof(m));
    ASSERT_TRUE(strstr(m, " /tmp/apps ramfs rw 0 0\n") != NULL);
    ASSERT_EQ(vfs_bind_mount("/tmp/none", "/tmp/apps2"), -ENOENT);
    ASSERT_EQ(vfs_bind_mount("/tmp/data/apps/tool", "/tmp/data"), -ENOTDIR);
    return 0;
}

/* --- Dentry cache --- */

TEST(dcache_caches_and_forgets) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/dc", 0755), 0);
    ASSERT_EQ(mkfile("/tmp/dc/f", "x"), 0);
    uint32_t n0 = dcache_count();
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/dc/f", &st), 0);
    uint32_t n1 = dcache_count();
    ASSERT_EQ(vfs_stat("/tmp/dc/f", &st), 0);
    ASSERT_EQ(dcache_count(), n1);   /* the second walk is all hits */
    ASSERT_TRUE(n1 >= n0);
    ASSERT_EQ(vfs_unlink("/tmp/dc/f"), 0);
    ASSERT_EQ(dcache_count(), n1 - 1);   /* the removed name went */
    ASSERT_EQ(vfs_stat("/tmp/dc/f", &st), -ENOENT);
    /* Long names are looked up every time, not cached */
    char path[64] = "/tmp/dc/";
    memset(path + 8, 'L', 40);
    ASSERT_EQ(mkfile(path, "x"), 0);
    uint32_t n2 = dcache_count();
    ASSERT_EQ(vfs_stat(path, &st), 0);
    ASSERT_EQ(dcache_count(), n2);
    return 0;
}

TEST(dcache_is_bounded) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/many", 0755), 0);
    char path[64];
    struct stat st;
    for (int i = 0; i < DCACHE_MAX + 100; i++) {
        snprintf(path, sizeof(path), "/tmp/many/f%d", i);
        ASSERT_EQ(vfs_write_file(path, "", 0, 0644), 0);
        ASSERT_EQ(vfs_stat(path, &st), 0);
    }
    ASSERT_TRUE(dcache_count() <= DCACHE_MAX);
    /* Evicted names still resolve (through the filesystem) */
    ASSERT_EQ(vfs_stat("/tmp/many/f0", &st), 0);
    return 0;
}

/* A write whose user buffer faults half-way keeps the bytes it copied and
 * leaves the rest of the file alone (D3 review of #41: it used to zero them) */
TEST(write_faulting_half_way_keeps_the_rest) {
    vfs_env_setup();
    char old[4096];
    memset(old, 'A', sizeof(old));
    ASSERT_EQ(vfs_write_file("/tmp/half", old, sizeof(old), 0644), 0);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/half", O_RDWR, 0, &f), 0);
    static char src[4096];
    memset(src, 'B', sizeof(src));
    vfs_env_fault_at = (uintptr_t)src + 2048;
    IoBuf ub = iob_user((uint64_t)(uintptr_t)src, sizeof(src));
    ASSERT_EQ(vfs_pwrite(f, &ub, 0), 2048);   /* the bytes before the fault */
    IoBuf none = iob_user((uint64_t)(uintptr_t)src + 2048, 16);
    ASSERT_EQ(vfs_pwrite(f, &none, 0), -EFAULT);   /* nothing copied: EFAULT, file unchanged */
    vfs_env_fault_at = 0;
    char back[4096];
    IoBuf rb = iob_kernel(back, sizeof(back));
    ASSERT_EQ(vfs_pread(f, &rb, 0), 4096);
    ASSERT_EQ(back[0], 'B');
    ASSERT_EQ(back[2047], 'B');
    ASSERT_EQ(back[2048], 'A');
    ASSERT_EQ(back[4095], 'A');
    /* Past the end: only what was copied counts toward the size */
    vfs_env_fault_at = (uintptr_t)src + 100;
    ASSERT_EQ(vfs_pwrite(f, &ub, 4096), 100);
    vfs_env_fault_at = 0;
    ASSERT_EQ(size_of("/tmp/half"), 4196);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/half"), 0);
    return 0;
}

/* mkdir in a directory removed between resolving and locking it fails
 * (simulated by marking the directory removed) */
TEST(no_mkdir_in_removed_directory) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/gone", 0755), 0);
    PathLoc loc;
    ASSERT_EQ(vfs_resolve("/tmp/gone", &loc), 0);
    __atomic_or_fetch(&loc.inode->flags, I_DEAD, __ATOMIC_RELAXED);
    ASSERT_EQ(vfs_mkdir("/tmp/gone/sub", 0755), -ENOENT);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/gone/f", O_CREAT | O_RDWR, 0644, &f), -ENOENT);
    ASSERT_EQ(mkfile("/tmp/rn_src", "x"), 0);
    ASSERT_EQ(vfs_rename("/tmp/rn_src", "/tmp/gone/f"), -ENOENT);
    __atomic_and_fetch(&loc.inode->flags, ~(uint32_t)I_DEAD, __ATOMIC_RELAXED);
    loc_put(&loc);
    ASSERT_EQ(vfs_unlink("/tmp/rn_src"), 0);
    ASSERT_EQ(vfs_rmdir("/tmp/gone"), 0);
    return 0;
}

/* exec needs execute permission, not read (0711 runs for others) */
TEST(exec_open_needs_x_not_r) {
    vfs_env_setup();
    ASSERT_EQ(vfs_write_file("/tmp/prog", "\177ELF", 4, 0711), 0);
    ASSERT_EQ(vfs_write_file("/tmp/noexec", "x", 1, 0644), 0);
    vfs_env_as(1000, 1000);
    File *f;
    ASSERT_EQ(vfs_open("/tmp/prog", O_RDONLY, 0, &f), -EACCES);   /* no read permission */
    ASSERT_EQ(vfs_open_exec("/tmp/prog", &f), 0);
    char buf[4];
    IoBuf b = iob_kernel(buf, 4);
    ASSERT_EQ(vfs_pread(f, &b, 0), 4);
    fput(f);
    ASSERT_EQ(vfs_open_exec("/tmp/noexec", &f), -EACCES);
    ASSERT_EQ(vfs_open_exec("/tmp", &f), -EACCES);   /* not a regular file */
    vfs_env_as(0, 0);
    ASSERT_EQ(vfs_unlink("/tmp/prog"), 0);
    ASSERT_EQ(vfs_unlink("/tmp/noexec"), 0);
    return 0;
}

/* --- Failures and leaks --- */

TEST(out_of_memory_leaves_nothing) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/oom", 0755), 0);
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/oom", &st), 0);   /* warm the dentry cache */
    VfsEnvUsage before, after;
    for (int n = 1; n <= 6; n++) {
        vfs_env_usage(&before);
        vfs_env_fail_alloc_in(n);
        File *f = NULL;
        int err = vfs_open("/tmp/oom/f", O_CREAT | O_RDWR, 0644, &f);
        vfs_env_fail_alloc_in(0);
        if (err == 0) {
            fput(f);
            ASSERT_EQ(vfs_unlink("/tmp/oom/f"), 0);
        } else {
            ASSERT_EQ(err, -ENOMEM);
            /* As on Linux, open may fail after the create: the empty file stays */
            if (vfs_stat("/tmp/oom/f", &st) == 0) ASSERT_EQ(vfs_unlink("/tmp/oom/f"), 0);
        }
        vfs_env_usage(&after);
        ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    }
    return 0;
}

TEST(create_write_remove_is_leak_free) {
    vfs_env_setup();
    ASSERT_EQ(vfs_mkdir("/tmp/lk", 0755), 0);
    struct stat st;
    ASSERT_EQ(vfs_stat("/tmp/lk", &st), 0);   /* warm the dentry cache */
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    for (int i = 0; i < 20; i++) {
        File *f;
        ASSERT_EQ(vfs_open("/tmp/lk/f", O_CREAT | O_RDWR, 0644, &f), 0);
        char big[6000];
        memset(big, 'b', sizeof(big));
        IoBuf b = iob_kernel(big, sizeof(big));
        ASSERT_EQ(vfs_write(f, &b), (int64_t)sizeof(big));
        fput(f);
        ASSERT_EQ(vfs_mkdir("/tmp/lk/d", 0755), 0);
        ASSERT_EQ(vfs_rename("/tmp/lk/f", "/tmp/lk/d/g"), 0);
        ASSERT_EQ(vfs_unlink("/tmp/lk/d/g"), 0);
        ASSERT_EQ(vfs_rmdir("/tmp/lk/d"), 0);
    }
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    return 0;
}

TestCase vfs_tests[] = {
    TEST_ENTRY(root_is_directory),
    TEST_ENTRY(create_write_read),
    TEST_ENTRY(write_faulting_half_way_keeps_the_rest),
    TEST_ENTRY(no_mkdir_in_removed_directory),
    TEST_ENTRY(exec_open_needs_x_not_r),
    TEST_ENTRY(open_flags),
    TEST_ENTRY(access_mode_enforced),
    TEST_ENTRY(append_writes_at_end),
    TEST_ENTRY(sparse_file_beyond_4gib),
    TEST_ENTRY(seek_and_offset_limits),
    TEST_ENTRY(shared_offset_between_references),
    TEST_ENTRY(bad_user_buffer_faults),
    TEST_ENTRY(write_updates_times),
    TEST_ENTRY(truncate_shrinks_and_zero_fills),
    TEST_ENTRY(setattr_rules),
    TEST_ENTRY(mkdir_and_nlink),
    TEST_ENTRY(getdents_lists_dots_then_entries),
    TEST_ENTRY(getdents_resumes_by_cookie),
    TEST_ENTRY(unlink_rules),
    TEST_ENTRY(unlinked_file_stays_readable_while_open),
    TEST_ENTRY(rmdir_rules),
    TEST_ENTRY(name_limits),
    TEST_ENTRY(rename_within_and_across_directories),
    TEST_ENTRY(rename_replaces_and_type_checks),
    TEST_ENTRY(rename_directory_moves_dotdot),
    TEST_ENTRY(walk_from_a_directory_and_rename_ancestry),
    TEST_ENTRY(permissions_on_create_and_open),
    TEST_ENTRY(search_permission_on_path),
    TEST_ENTRY(sticky_directory_protects_entries),
    TEST_ENTRY(umask_applies),
    TEST_ENTRY(mounts_and_proc_mounts),
    TEST_ENTRY(mount_covers_directory),
    TEST_ENTRY(bind_mount_shares_subtree),
    TEST_ENTRY(dcache_caches_and_forgets),
    TEST_ENTRY(dcache_is_bounded),
    TEST_ENTRY(out_of_memory_leaves_nothing),
    TEST_ENTRY(create_write_remove_is_leak_free),
};
int vfs_test_count = sizeof(vfs_tests) / sizeof(vfs_tests[0]);
