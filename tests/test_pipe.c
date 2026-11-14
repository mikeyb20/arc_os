/* arc_os — pipes on the real pipe.c (M1-11): data order, EOF and EPIPE,
 * O_NONBLOCK, PIPE_BUF atomicity, blocking through the wait-queue hook, no
 * data lost on -EFAULT, streams refusing seeks, and freeing on last close. */

#include "test_framework.h"
#include "vfs_env.h"

#include "fs/pipe.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

static int64_t pw(File *w, const char *s, size_t n) {
    IoBuf b = iob_kernel((void *)s, n);
    return vfs_write(w, &b);
}

static int64_t pr(File *r, char *buf, size_t n) {
    IoBuf b = iob_kernel(buf, n);
    return vfs_read(r, &b);
}

TEST(create_gives_fifo_ends) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    ASSERT_TRUE(S_ISFIFO(r->inode->mode));
    ASSERT_TRUE(r->inode == w->inode);
    ASSERT_EQ(r->flags & O_ACCMODE, O_RDONLY);
    ASSERT_EQ(w->flags & O_ACCMODE, O_WRONLY);
    char c;
    ASSERT_EQ(pw(r, "x", 1), -EBADF);   /* wrong ends */
    ASSERT_EQ(pr(w, &c, 1), -EBADF);
    ASSERT_EQ(vfs_llseek(r, 0, SEEK_SET), -ESPIPE);
    IoBuf b = iob_kernel(&c, 1);
    ASSERT_EQ(vfs_pread(r, &b, 0), -ESPIPE);
    ASSERT_EQ(vfs_pwrite(w, &b, 0), -ESPIPE);
    fput(r);
    fput(w);
    return 0;
}

TEST(bytes_come_out_in_order) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    ASSERT_EQ(pw(w, "abc", 3), 3);
    ASSERT_EQ(pw(w, "defg", 4), 4);
    char buf[16] = {0};
    ASSERT_EQ(pr(r, buf, 2), 2);
    ASSERT_MEM_EQ(buf, "ab", 2);
    ASSERT_EQ(pr(r, buf, sizeof(buf)), 5);
    ASSERT_MEM_EQ(buf, "cdefg", 5);
    ASSERT_EQ(pr(r, buf, 0), 0);
    ASSERT_EQ(pw(w, buf, 0), 0);
    fput(r);
    fput(w);
    return 0;
}

TEST(eof_and_epipe) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    ASSERT_EQ(pw(w, "z", 1), 1);
    fput(w);
    char c;
    ASSERT_EQ(pr(r, &c, 1), 1);   /* buffered bytes first */
    ASSERT_EQ(pr(r, &c, 1), 0);   /* then end of file */
    fput(r);
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    fput(r);
    ASSERT_EQ(pw(w, "z", 1), -EPIPE);
    fput(w);
    return 0;
}

TEST(nonblocking_and_atomic_writes) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, O_NONBLOCK), 0);
    char buf[PIPE_BUF_SIZE + 8];
    ASSERT_EQ(pr(r, buf, 1), -EAGAIN);
    memset(buf, 'p', sizeof(buf));
    ASSERT_EQ(pw(w, buf, PIPE_BUF_SIZE - 10), PIPE_BUF_SIZE - 10);
    ASSERT_EQ(pw(w, buf, 20), -EAGAIN);   /* <= PIPE_BUF: all or nothing */
    ASSERT_EQ(pw(w, buf, 10), 10);         /* exactly full */
    ASSERT_EQ(pw(w, buf, 1), -EAGAIN);
    ASSERT_EQ(pr(r, buf, sizeof(buf)), PIPE_BUF_SIZE);
    /* A big write may be split */
    ASSERT_EQ(pw(w, buf, PIPE_BUF_SIZE + 8), PIPE_BUF_SIZE);
    fput(r);
    fput(w);
    return 0;
}

static File *hook_w;
static void write_when_asleep(void) {
    pw(hook_w, "late", 4);
}

static File *hook_r;
static void drain_when_asleep(void) {
    char buf[PIPE_BUF_SIZE];
    pr(hook_r, buf, sizeof(buf));
}

TEST(blocking_read_and_write_wait) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    hook_w = w;
    vfs_env_sleep_hook = write_when_asleep;
    int sleeps = vfs_env_sleeps;
    char buf[PIPE_BUF_SIZE];
    ASSERT_EQ(pr(r, buf, sizeof(buf)), 4);   /* slept once, then got the bytes */
    ASSERT_EQ(vfs_env_sleeps, sleeps + 1);
    ASSERT_MEM_EQ(buf, "late", 4);
    memset(buf, 'f', sizeof(buf));
    ASSERT_EQ(pw(w, buf, PIPE_BUF_SIZE), PIPE_BUF_SIZE);
    hook_r = r;
    vfs_env_sleep_hook = drain_when_asleep;
    ASSERT_EQ(pw(w, "more", 4), 4);   /* full: slept until the reader drained it */
    ASSERT_EQ(vfs_env_sleeps, sleeps + 2);
    vfs_env_sleep_hook = NULL;
    fput(r);
    fput(w);
    return 0;
}

/* A reader blocked on an empty pipe doesn't hold the read side: an
 * O_NONBLOCK read meanwhile gets -EAGAIN at once (D3 review of #41). The
 * mutex stubs abort if the blocked reader still held rd_mutex. */
static File *hook_nb;
static int hook_nb_result;
static void nonblocking_read_while_asleep(void) {
    char c;
    hook_nb->flags |= O_NONBLOCK;
    hook_nb_result = (int)pr(hook_nb, &c, 1);
    hook_nb->flags &= ~(uint32_t)O_NONBLOCK;
    vfs_env_sleep_hook = NULL;
    pw(hook_w, "z", 1);
}

TEST(blocked_reader_holds_nothing) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    hook_nb = r;
    hook_w = w;
    hook_nb_result = 0;
    vfs_env_sleep_hook = nonblocking_read_while_asleep;
    char c;
    ASSERT_EQ(pr(r, &c, 1), 1);
    ASSERT_EQ(c, 'z');
    ASSERT_EQ(hook_nb_result, -EAGAIN);
    vfs_env_sleep_hook = NULL;
    fput(r);
    fput(w);
    return 0;
}

TEST(wraparound_keeps_order) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, O_NONBLOCK), 0);
    char out[3000], in[3000];
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 3000; i++) out[i] = (char)(i * 7 + round);
        ASSERT_EQ(pw(w, out, sizeof(out)), 3000);
        ASSERT_EQ(pr(r, in, sizeof(in)), 3000);
        ASSERT_MEM_EQ(in, out, sizeof(out));
    }
    fput(r);
    fput(w);
    return 0;
}

TEST(efault_loses_no_data) {
    vfs_env_setup();
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    ASSERT_EQ(pw(w, "keep", 4), 4);
    IoBuf bad = iob_user(VFS_ENV_BAD_UADDR, 4);
    ASSERT_EQ(vfs_read(r, &bad), -EFAULT);
    char buf[8];
    ASSERT_EQ(pr(r, buf, sizeof(buf)), 4);
    ASSERT_MEM_EQ(buf, "keep", 4);
    ASSERT_EQ(vfs_write(w, &bad), -EFAULT);
    fput(r);
    fput(w);
    return 0;
}

TEST(shared_ends_and_freeing) {
    vfs_env_setup();
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    File *r, *w;
    ASSERT_EQ(pipe_create(&r, &w, 0), 0);
    File *w2 = fget_ref(w);   /* a dup of the write end */
    fput(w);
    ASSERT_EQ(pw(w2, "x", 1), 1);   /* still a writer: no EOF */
    char c;
    ASSERT_EQ(pr(r, &c, 1), 1);
    fput(w2);
    ASSERT_EQ(pr(r, &c, 1), 0);
    fput(r);
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    return 0;
}

TEST(out_of_memory) {
    vfs_env_setup();
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    for (int n = 1; n <= 5; n++) {
        File *r = NULL, *w = NULL;
        vfs_env_fail_alloc_in(n);
        int err = pipe_create(&r, &w, 0);
        vfs_env_fail_alloc_in(0);
        if (err == 0) {
            fput(r);
            fput(w);
        } else {
            ASSERT_EQ(err, -ENOMEM);
        }
        vfs_env_usage(&after);
        ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    }
    return 0;
}

TestCase pipe_tests[] = {
    TEST_ENTRY(create_gives_fifo_ends),
    TEST_ENTRY(bytes_come_out_in_order),
    TEST_ENTRY(eof_and_epipe),
    TEST_ENTRY(nonblocking_and_atomic_writes),
    TEST_ENTRY(blocking_read_and_write_wait),
    TEST_ENTRY(blocked_reader_holds_nothing),
    TEST_ENTRY(wraparound_keeps_order),
    TEST_ENTRY(efault_loses_no_data),
    TEST_ENTRY(shared_ends_and_freeing),
    TEST_ENTRY(out_of_memory),
};
int pipe_test_count = sizeof(pipe_tests) / sizeof(pipe_tests[0]);
