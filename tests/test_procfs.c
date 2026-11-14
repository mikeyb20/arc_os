/* arc_os — /proc on the real procfs.c (M1-11): fixed files, per-process
 * directories made per lookup and freed after, /proc/self, /proc/mounts,
 * partial reads, kmsg permissions, the root listing and
 * /proc/sys/vm/drop_caches. */

#include "test_framework.h"
#include "vfs_env.h"

#include "fs/vfs_internal.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

static char text[4096];

typedef struct {
    DirContext ctx;
    char names[512];
} Names;

static int names_actor(DirContext *ctx, const char *name, size_t len, uint64_t ino, uint8_t dtype, loff_t next) {
    (void)ino, (void)dtype, (void)next;
    Names *n = (Names *)ctx;
    size_t at = strlen(n->names);
    snprintf(n->names + at, sizeof(n->names) - at, "%s%.*s", at ? " " : "", (int)len, name);
    return 0;
}

static int list(const char *path, Names *n) {
    File *d;
    int err = vfs_open(path, O_RDONLY | O_DIRECTORY, 0, &d);
    if (err != 0) return err;
    memset(n, 0, sizeof(*n));
    n->ctx.actor = names_actor;
    err = vfs_iterate(d, &n->ctx);
    fput(d);
    return err;
}

TEST(meminfo_fields) {
    vfs_env_setup();
    ASSERT_TRUE(vfs_env_read_file("/proc/meminfo", text, sizeof(text)) > 0);
    ASSERT_TRUE(strstr(text, "MemTotal: 4096 kB\n") != NULL);
    ASSERT_TRUE(strstr(text, "HeapUsed: ") != NULL);
    ASSERT_TRUE(strstr(text, "HeapLarge: ") != NULL);
    return 0;
}

TEST(fixed_files) {
    vfs_env_setup();
    vfs_env_uptime_ms = 12340;
    ASSERT_TRUE(vfs_env_read_file("/proc/uptime", text, sizeof(text)) > 0);
    ASSERT_TRUE(strncmp(text, "12.34", 5) == 0);
    ASSERT_TRUE(vfs_env_read_file("/proc/cmdline", text, sizeof(text)) > 0);
    ASSERT_STR_EQ(text, "arc.test=vfs\n");
    ASSERT_TRUE(vfs_env_read_file("/proc/cpuinfo", text, sizeof(text)) > 0);
    ASSERT_TRUE(strstr(text, "processor") != NULL);
    ASSERT_TRUE(vfs_env_read_file("/proc/slabinfo", text, sizeof(text)) > 0);
    ASSERT_TRUE(strstr(text, "host") != NULL);
    ASSERT_TRUE(vfs_env_read_file("/proc/sched_debug", text, sizeof(text)) > 0);
    ASSERT_TRUE(vfs_env_read_file("/proc/kmsg", text, sizeof(text)) > 0);
    ASSERT_STR_EQ(text, "<6>[BOOT] host\n");
    /* /proc/interrupts (M1-09): the IRQ layer's text, passed through */
    ASSERT_TRUE(vfs_env_read_file("/proc/interrupts", text, sizeof(text)) > 0);
    ASSERT_TRUE(strstr(text, "PCI-MSIX 0000:00:04.0") != NULL);
    ASSERT_TRUE(strstr(text, " ERR: 0\n") != NULL);
    struct stat st;
    ASSERT_EQ(vfs_stat("/proc/meminfo", &st), 0);
    ASSERT_EQ(st.st_mode, S_IFREG | 0444);
    ASSERT_EQ(vfs_stat("/proc/nothing", &st), -ENOENT);
    return 0;
}

TEST(proc_mounts_lists_the_table) {
    vfs_env_setup();
    ASSERT_TRUE(vfs_env_read_file("/proc/mounts", text, sizeof(text)) > 0);
    ASSERT_TRUE(strncmp(text, "ramfs / ramfs rw 0 0\n", 21) == 0);
    ASSERT_TRUE(strstr(text, "proc /proc proc rw 0 0\n") != NULL);
    return 0;
}

TEST(partial_reads_continue) {
    vfs_env_setup();
    char whole[1024];
    int64_t total = vfs_env_read_file("/proc/meminfo", whole, sizeof(whole));
    ASSERT_TRUE(total > 20);
    File *f;
    ASSERT_EQ(vfs_open("/proc/meminfo", O_RDONLY, 0, &f), 0);
    char parts[1024];
    int64_t got = 0;
    for (;;) {
        IoBuf b = iob_kernel(parts + got, 7);
        int64_t n = vfs_read(f, &b);
        ASSERT_TRUE(n >= 0);
        if (n == 0) break;
        got += n;
    }
    fput(f);
    ASSERT_EQ(got, total);
    ASSERT_MEM_EQ(parts, whole, (size_t)total);
    return 0;
}

TEST(kmsg_is_root_only) {
    vfs_env_setup();
    vfs_env_as(1000, 1000);
    File *f;
    ASSERT_EQ(vfs_open("/proc/kmsg", O_RDONLY, 0, &f), -EACCES);
    ASSERT_EQ(vfs_open("/proc/meminfo", O_RDONLY, 0, &f), 0);
    fput(f);
    ASSERT_EQ(vfs_open("/proc/meminfo", O_WRONLY, 0, &f), -EACCES);
    vfs_env_as(0, 0);
    return 0;
}

TEST(pid_directory_files) {
    vfs_env_setup();
    static char fake_mm;
    vfs_env_proc.mm = (struct Mm *)&fake_mm;
    ASSERT_TRUE(vfs_env_read_file("/proc/1/status", text, sizeof(text)) > 0);
    ASSERT_TRUE(strncmp(text, "Pid: 1\n", 7) == 0);
    ASSERT_TRUE(vfs_env_read_file("/proc/self/status", text, sizeof(text)) > 0);
    ASSERT_TRUE(strncmp(text, "Pid: 1\n", 7) == 0);
    ASSERT_TRUE(vfs_env_read_file("/proc/1/maps", text, sizeof(text)) > 0);
    ASSERT_TRUE(strstr(text, "r-xp") != NULL);
    vfs_env_proc.mm = NULL;
    struct stat st;
    ASSERT_EQ(vfs_stat("/proc/2", &st), -ENOENT);
    ASSERT_EQ(vfs_stat("/proc/1x", &st), -ENOENT);
    ASSERT_EQ(vfs_stat("/proc/1/none", &st), -ENOENT);
    ASSERT_EQ(vfs_stat("/proc/1", &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    struct stat status;
    ASSERT_EQ(vfs_stat("/proc/1/status", &status), 0);
    ASSERT_NEQ(status.st_ino, st.st_ino);   /* every proc inode has its own number */
    ASSERT_NEQ(status.st_ino, 2);
    Names n;
    ASSERT_EQ(list("/proc/1", &n), 0);
    ASSERT_STR_EQ(n.names, ". .. status maps");
    return 0;
}

TEST(pid_entries_are_freed) {
    vfs_env_setup();
    struct stat st;
    ASSERT_EQ(vfs_stat("/proc/1/status", &st), 0);   /* warm up the path to /proc */
    VfsEnvUsage before, after;
    vfs_env_usage(&before);
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(vfs_env_read_file("/proc/1/status", text, sizeof(text)) > 0);
        ASSERT_EQ(vfs_stat("/proc/self", &st), 0);
    }
    vfs_env_usage(&after);
    ASSERT_TRUE(vfs_env_no_leak(&before, &after));
    return 0;
}

TEST(root_listing_and_no_new_names) {
    vfs_env_setup();
    Names n;
    ASSERT_EQ(list("/proc", &n), 0);
    ASSERT_STR_EQ(n.names,
                  ". .. meminfo uptime cmdline cpuinfo slabinfo kmsg sched_debug mounts interrupts vmstat sys self 1");
    File *f;
    ASSERT_EQ(vfs_open("/proc/new", O_CREAT | O_WRONLY, 0644, &f), -EACCES);   /* no create op */
    ASSERT_EQ(vfs_mkdir("/proc/d", 0755), -EPERM);                            /* no mkdir op */
    vfs_env_as(1000, 1000);
    ASSERT_EQ(vfs_mkdir("/proc/d", 0755), -EACCES);   /* 0555 root: no write permission first */
    vfs_env_as(0, 0);
    return 0;
}

static int64_t write_str(const char *path, const char *s) {
    File *f;
    int err = vfs_open(path, O_WRONLY, 0, &f);
    if (err != 0) return err;
    IoBuf b = iob_kernel((void *)s, strlen(s));
    int64_t n = vfs_write(f, &b);
    fput(f);
    return n;
}

/* Writing 2 empties the dentry cache; only root may; bad values are EINVAL */
TEST(drop_caches_empties_the_dentry_cache) {
    vfs_env_setup();
    Names n;
    ASSERT_EQ(list("/proc/sys", &n), 0);
    ASSERT_STR_EQ(n.names, ". .. vm");
    ASSERT_EQ(list("/proc/sys/vm", &n), 0);
    ASSERT_STR_EQ(n.names, ". .. drop_caches");
    struct stat st;
    ASSERT_EQ(vfs_stat("/proc/sys/vm/drop_caches", &st), 0);
    ASSERT_EQ(st.st_mode, S_IFREG | 0200);

    ASSERT_EQ(vfs_mkdir("/tmp/proc_dc", 0755), 0);
    ASSERT_EQ(vfs_stat("/tmp/proc_dc", &st), 0);   /* a lookup: /tmp and dc are cached */
    ASSERT_TRUE(dcache_count() > 0);
    ASSERT_EQ(write_str("/proc/sys/vm/drop_caches", "2\n"), 2);
    ASSERT_EQ(dcache_count(), 0);
    ASSERT_EQ(vfs_stat("/tmp/proc_dc", &st), 0);   /* names still resolve, through the filesystem */
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_EQ(write_str("/proc/sys/vm/drop_caches", "1"), 1);   /* page cache: nothing yet */
    ASSERT_EQ(write_str("/proc/sys/vm/drop_caches", "4"), 1);   /* Linux's "quiet" bit: nothing to drop */
    ASSERT_EQ(write_str("/proc/sys/vm/drop_caches", "5"), -EINVAL);
    ASSERT_EQ(write_str("/proc/sys/vm/drop_caches", "x"), -EINVAL);
    ASSERT_EQ(write_str("/proc/sys/vm/drop_caches", ""), 0);   /* an empty write does nothing */
    File *f;
    ASSERT_EQ(vfs_open("/proc/sys/vm/drop_caches", O_RDONLY, 0, &f), 0);   /* root passes the mode bits */
    char c;
    IoBuf rb = iob_kernel(&c, 1);
    ASSERT_EQ(vfs_read(f, &rb), -EINVAL);   /* but there is nothing to read */
    fput(f);
    vfs_env_as(1000, 1000);
    ASSERT_EQ(write_str("/proc/sys/vm/drop_caches", "2"), -EACCES);
    vfs_env_as(0, 0);
    ASSERT_EQ(vfs_rmdir("/tmp/proc_dc"), 0);
    return 0;
}

TestCase procfs_tests[] = {
    TEST_ENTRY(meminfo_fields),
    TEST_ENTRY(fixed_files),
    TEST_ENTRY(proc_mounts_lists_the_table),
    TEST_ENTRY(partial_reads_continue),
    TEST_ENTRY(kmsg_is_root_only),
    TEST_ENTRY(pid_directory_files),
    TEST_ENTRY(pid_entries_are_freed),
    TEST_ENTRY(root_listing_and_no_new_names),
    TEST_ENTRY(drop_caches_empties_the_dentry_cache),
};
int procfs_test_count = sizeof(procfs_tests) / sizeof(procfs_tests[0]);
