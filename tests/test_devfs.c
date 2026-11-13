/* arc_os — /dev on the real devfs.c (M1-11): null, zero, the terminal, the
 * console File, ioctls, the listing, and refusing new names. */

#include "test_framework.h"
#include "vfs_env.h"

#include "fs/devfs.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/termios.h"


typedef struct {
    DirContext ctx;
    char names[128];
} Names;

static int names_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    (void)ino, (void)dtype, (void)next;
    Names *n = (Names *)ctx;
    size_t at = strlen(n->names);
    snprintf(n->names + at, sizeof(n->names) - at, "%s%.*s", at ? " " : "", (int)len, name);
    return 0;
}

TEST(nodes_are_character_devices) {
    vfs_env_setup();
    struct stat st;
    ASSERT_EQ(vfs_stat("/dev/null", &st), 0);
    ASSERT_EQ(st.st_mode, S_IFCHR | 0666);
    ASSERT_EQ(st.st_rdev, (1 << 8) | 3);
    ASSERT_EQ(vfs_stat("/dev/zero", &st), 0);
    ASSERT_EQ(st.st_rdev, (1 << 8) | 5);
    ASSERT_EQ(vfs_stat("/dev/tty", &st), 0);
    ASSERT_EQ(st.st_rdev, (5 << 8) | 0);
    ASSERT_EQ(vfs_stat("/dev/none", &st), -ENOENT);
    return 0;
}

TEST(null_and_zero) {
    vfs_env_setup();
    File *f;
    char buf[16];
    ASSERT_EQ(vfs_open("/dev/null", O_RDWR, 0, &f), 0);
    IoBuf b = iob_kernel(buf, sizeof(buf));
    ASSERT_EQ(vfs_read(f, &b), 0);
    ASSERT_EQ(vfs_write(f, &b), 16);
    ASSERT_EQ(vfs_llseek(f, 100, SEEK_SET), 0);
    fput(f);
    ASSERT_EQ(vfs_open("/dev/zero", O_RDONLY, 0, &f), 0);
    memset(buf, 'x', sizeof(buf));
    ASSERT_EQ(vfs_read(f, &b), 16);
    for (int i = 0; i < 16; i++) ASSERT_EQ(buf[i], 0);
    IoBuf r = iob_kernel(buf, 4);
    ASSERT_EQ(vfs_pread(f, &r, 1ll << 40), 4);   /* any offset */
    fput(f);
    return 0;
}

TEST(tty_reads_and_writes_the_terminal) {
    vfs_env_setup();
    File *f;
    ASSERT_EQ(vfs_open("/dev/tty", O_RDWR, 0, &f), 0);
    vfs_env_tty_out_len = 0;
    IoBuf w = iob_kernel("hi\n", 3);
    ASSERT_EQ(vfs_write(f, &w), 3);
    ASSERT_EQ(vfs_env_tty_out_len, 3);
    ASSERT_MEM_EQ(vfs_env_tty_out, "hi\n", 3);
    vfs_env_tty_input("typed\n");
    char buf[16];
    IoBuf r = iob_kernel(buf, sizeof(buf));
    ASSERT_EQ(vfs_read(f, &r), 6);
    ASSERT_MEM_EQ(buf, "typed\n", 6);
    ASSERT_EQ(vfs_llseek(f, 0, SEEK_SET), -ESPIPE);
    /* Longer than one bounce chunk */
    char big[1300];
    memset(big, 'b', sizeof(big));
    vfs_env_tty_out_len = 0;
    IoBuf wb = iob_kernel(big, sizeof(big));
    ASSERT_EQ(vfs_write(f, &wb), 1300);
    ASSERT_EQ(vfs_env_tty_out_len, 1300);
    fput(f);
    return 0;
}

TEST(console_is_the_terminal) {
    vfs_env_setup();
    File *c = devfs_console();
    ASSERT_TRUE(c != NULL);
    struct stat st;
    vfs_fill_stat(c->inode, &st);
    ASSERT_EQ(st.st_rdev, (5 << 8) | 0);
    ASSERT_EQ(c->flags & O_ACCMODE, O_RDWR);
    fput(c);
    File *again = devfs_console();   /* never freed: the console keeps its own reference */
    ASSERT_TRUE(again == c);
    fput(again);
    return 0;
}

TEST(tty_ioctls) {
    vfs_env_setup();
    File *f;
    ASSERT_EQ(vfs_open("/dev/tty", O_RDWR, 0, &f), 0);
    struct winsize ws;
    ASSERT_EQ(devfs_tty_ioctl(f, TIOCGWINSZ, (uint64_t)(uintptr_t)&ws), 0);
    ASSERT_EQ(ws.ws_col, 80);
    int32_t pgid = 7;
    ASSERT_EQ(devfs_tty_ioctl(f, TIOCSPGRP, (uint64_t)(uintptr_t)&pgid), 0);
    pgid = 0;
    ASSERT_EQ(devfs_tty_ioctl(f, TIOCGPGRP, (uint64_t)(uintptr_t)&pgid), 0);
    ASSERT_EQ(pgid, 7);
    pgid = -1;
    ASSERT_EQ(devfs_tty_ioctl(f, TIOCSPGRP, (uint64_t)(uintptr_t)&pgid), -EINVAL);
    ASSERT_EQ(devfs_tty_ioctl(f, TIOCGWINSZ, VFS_ENV_BAD_UADDR), -EFAULT);
    ASSERT_EQ(devfs_tty_ioctl(f, 0x1234, 0), -ENOTTY);
    struct termios t;
    ASSERT_EQ(devfs_tty_ioctl(f, TCGETS, (uint64_t)(uintptr_t)&t), 0);
    fput(f);
    return 0;
}

TEST(listing_and_no_new_names) {
    vfs_env_setup();
    File *d;
    ASSERT_EQ(vfs_open("/dev", O_RDONLY | O_DIRECTORY, 0, &d), 0);
    Names n;
    memset(&n, 0, sizeof(n));
    n.ctx.actor = names_actor;
    ASSERT_EQ(vfs_iterate(d, &n.ctx), 0);
    ASSERT_STR_EQ(n.names, ". .. null zero tty");
    fput(d);
    File *f;
    ASSERT_EQ(vfs_open("/dev/newdev", O_CREAT | O_WRONLY, 0644, &f), -EACCES);
    ASSERT_EQ(vfs_mkdir("/dev/sub", 0755), -EPERM);
    ASSERT_EQ(vfs_unlink("/dev/null"), -EPERM);
    return 0;
}

TestCase devfs_tests[] = {
    TEST_ENTRY(nodes_are_character_devices), TEST_ENTRY(null_and_zero), TEST_ENTRY(tty_reads_and_writes_the_terminal),
    TEST_ENTRY(console_is_the_terminal),     TEST_ENTRY(tty_ioctls),    TEST_ENTRY(listing_and_no_new_names),
};
int devfs_test_count = sizeof(devfs_tests) / sizeof(devfs_tests[0]);
