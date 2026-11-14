/* arc_os — fd tables on the real fd.c and File references (M1-11): lowest
 * free fd, min_fd, EMFILE, install_at replacing, close-on-exec, fork copies
 * sharing Files, and the last table owner closing everything. */

#include "test_framework.h"
#include "vfs_env.h"

#include "proc/fd.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

static int releases;

static void count_release(Inode *inode, File *file) {
    (void)inode;
    (void)file;
    releases++;
}

static const FileOps counting_fops = {.fflags = FOP_STREAM, .release = count_release};

/* A File whose last fput bumps `releases` */
static File *new_file(void) {
    Inode *inode = inode_new(vfs_anon_sb(), 1, S_IFIFO | 0600);
    return inode != NULL ? file_new(inode, &counting_fops, O_RDWR) : NULL;
}

TEST(install_takes_lowest_free) {
    vfs_env_setup();
    FdTable *t = fd_table_new();
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 0);
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 1);
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 2);
    ASSERT_EQ(fd_close(t, 1), 0);
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 1);
    ASSERT_EQ(fd_install(t, new_file(), 10, 0), 10);   /* F_DUPFD's minimum */
    ASSERT_EQ(fd_install(t, new_file(), 10, 0), 11);
    File *f = new_file();
    ASSERT_EQ(fd_install(t, f, MAX_FDS, 0), -EINVAL);
    ASSERT_EQ(fd_install(t, f, -1, 0), -EINVAL);
    fput(f);
    releases = 0;
    fd_table_put(t);
    ASSERT_EQ(releases, 5);   /* fds 0, 1, 2, 10, 11 */
    return 0;
}

TEST(full_table_gives_emfile) {
    vfs_env_setup();
    FdTable *t = fd_table_new();
    for (int i = 0; i < MAX_FDS; i++) ASSERT_EQ(fd_install(t, new_file(), 0, 0), i);
    File *f = new_file();
    ASSERT_EQ(fd_install(t, f, 0, 0), -EMFILE);   /* the reference is still ours */
    releases = 0;
    fput(f);
    ASSERT_EQ(releases, 1);
    ASSERT_EQ(fd_close(t, 63), 0);
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 63);
    fd_table_put(t);
    return 0;
}

TEST(get_and_close) {
    vfs_env_setup();
    FdTable *t = fd_table_new();
    File *f = new_file();
    ASSERT_EQ(fd_install(t, f, 0, 0), 0);
    File *g = fd_get(t, 0);
    ASSERT_TRUE(g == f);
    ASSERT_EQ(f->refcount, 2);   /* fd_get hands out a reference */
    fput(g);
    ASSERT_TRUE(fd_get(t, 1) == NULL);
    ASSERT_TRUE(fd_get(t, -1) == NULL);
    ASSERT_TRUE(fd_get(t, MAX_FDS) == NULL);
    ASSERT_EQ(fd_close(t, 5), -EBADF);
    ASSERT_EQ(fd_close(t, MAX_FDS), -EBADF);
    releases = 0;
    ASSERT_EQ(fd_close(t, 0), 0);
    ASSERT_EQ(releases, 1);
    ASSERT_EQ(fd_close(t, 0), -EBADF);
    fd_table_put(t);
    return 0;
}

TEST(install_at_replaces) {
    vfs_env_setup();
    FdTable *t = fd_table_new();
    File *a = new_file();
    ASSERT_EQ(fd_install(t, a, 0, 0), 0);
    ASSERT_EQ(fd_install_at(t, 5, fget_ref(a), 1), 0);   /* dup3(0, 5, O_CLOEXEC) */
    ASSERT_EQ(fd_get_cloexec(t, 5), 1);
    ASSERT_EQ(fd_get_cloexec(t, 0), 0);
    releases = 0;
    ASSERT_EQ(fd_install_at(t, 0, new_file(), 0), 0);   /* dup2 over fd 0: a survives at 5 */
    ASSERT_EQ(releases, 0);
    ASSERT_EQ(fd_close(t, 5), 0);
    ASSERT_EQ(releases, 1);                               /* now a is gone */
    File *b = new_file();
    ASSERT_EQ(fd_install_at(t, MAX_FDS, b, 0), -EBADF);
    fput(b);
    fd_table_put(t);
    return 0;
}

TEST(cloexec_flags_and_exec) {
    vfs_env_setup();
    FdTable *t = fd_table_new();
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 0);
    ASSERT_EQ(fd_install(t, new_file(), 0, 1), 1);
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 2);
    ASSERT_EQ(fd_set_cloexec(t, 2, 1), 0);
    ASSERT_EQ(fd_set_cloexec(t, 7, 1), -EBADF);
    ASSERT_EQ(fd_get_cloexec(t, 7), -EBADF);
    ASSERT_EQ(fd_set_cloexec(t, 1, 0), 0);
    releases = 0;
    fd_close_on_exec(t);
    ASSERT_EQ(releases, 1);   /* only fd 2 */
    ASSERT_TRUE(fd_get(t, 2) == NULL);
    File *f = fd_get(t, 1);
    ASSERT_TRUE(f != NULL);
    fput(f);
    /* A reused fd starts without close-on-exec */
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 2);
    ASSERT_EQ(fd_get_cloexec(t, 2), 0);
    fd_table_put(t);
    return 0;
}

TEST(fork_copy_shares_files) {
    vfs_env_setup();
    FdTable *parent = fd_table_new();
    File *f = new_file();
    ASSERT_EQ(fd_install(parent, f, 0, 0), 0);
    ASSERT_EQ(fd_install(parent, new_file(), 0, 1), 1);
    FdTable *child = fd_table_dup(parent);
    ASSERT_TRUE(child != NULL);
    ASSERT_EQ(f->refcount, 2);
    ASSERT_EQ(fd_get_cloexec(child, 1), 1);
    File *cf = fd_get(child, 0);
    ASSERT_TRUE(cf == f);   /* the same open file: one offset */
    fput(cf);
    releases = 0;
    fd_table_put(child);
    ASSERT_EQ(releases, 0);   /* the parent still holds both */
    fd_table_put(parent);
    ASSERT_EQ(releases, 2);
    return 0;
}

TEST(shared_table_closes_on_last_put) {
    vfs_env_setup();
    FdTable *t = fd_table_new();
    ASSERT_EQ(fd_install(t, new_file(), 0, 0), 0);
    fd_table_get(t);
    releases = 0;
    fd_table_put(t);
    ASSERT_EQ(releases, 0);
    fd_table_put(t);
    ASSERT_EQ(releases, 1);
    fd_table_put(NULL);
    return 0;
}

TEST(tables_are_leak_free) {
    vfs_env_setup();
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    FdTable *t = fd_table_new();
    for (int i = 0; i < 10; i++) fd_install(t, new_file(), 0, i & 1);
    FdTable *c = fd_table_dup(t);
    fd_close_on_exec(c);
    fd_table_put(c);
    fd_table_put(t);
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    return 0;
}

TestCase fd_tests[] = {
    TEST_ENTRY(install_takes_lowest_free),
    TEST_ENTRY(full_table_gives_emfile),
    TEST_ENTRY(get_and_close),
    TEST_ENTRY(install_at_replaces),
    TEST_ENTRY(cloexec_flags_and_exec),
    TEST_ENTRY(fork_copy_shares_files),
    TEST_ENTRY(shared_table_closes_on_last_put),
    TEST_ENTRY(tables_are_leak_free),
};
int fd_test_count = sizeof(fd_tests) / sizeof(fd_tests[0]);
