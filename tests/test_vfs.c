/* arc_os — Host-side tests for VFS + ramfs */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that we stub */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_MEM_H        /* Use libc memcpy/memset */
#define ARCHOS_LIB_STRING_H     /* Use libc string functions */
#define ARCHOS_PROC_PROCESS_H   /* We define our own minimal Process */

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Allocation flags */
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01

/* kmalloc/kfree stubs using libc malloc */
static int kmalloc_fail_after;  /* If > 0, fail the Nth kmalloc call */
static int kmalloc_call_seq;

static void *kmalloc(size_t size, uint32_t flags) {
    kmalloc_call_seq++;
    if (kmalloc_fail_after > 0 && kmalloc_call_seq >= kmalloc_fail_after) {
        return NULL;
    }
    void *p = malloc(size);
    if (p && (flags & GFP_ZERO)) {
        memset(p, 0, size);
    }
    return p;
}

static void kfree(void *ptr) {
    free(ptr);
}

static void *krealloc(void *ptr, size_t new_size) {
    return realloc(ptr, new_size);
}

/* proc_current stub — controllable for permission tests */
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef struct Process {
    uint32_t pid;
    uid_t uid;
    gid_t gid;
    uid_t euid;
    gid_t egid;
} Process;
static Process *vfs_test_proc_ptr = NULL;
static Process *proc_current(void) { return vfs_test_proc_ptr; }

/* Include the implementations directly */
#include "../kernel/fs/vfs.c"
#include "../kernel/fs/ramfs.c"

/* Helper: reset VFS + ramfs state for each test */
static void reset_vfs(void) {
    vfs_root = NULL;
    mount_count = 0;
    memset(mount_table, 0, sizeof(mount_table));
    next_inode = 1;
    kmalloc_fail_after = 0;
    kmalloc_call_seq = 0;
    vfs_test_proc_ptr = NULL;
}

static void setup_vfs(void) {
    reset_vfs();
    vfs_init();
    VfsNode *root = ramfs_init();
    vfs_set_root(root);
}

/* --- Tests --- */

static int test_vfs_init_has_root(void) {
    setup_vfs();
    VfsNode *root = vfs_get_root();
    ASSERT_TRUE(root != NULL);
    ASSERT_EQ(root->type, VFS_DIRECTORY);
    ASSERT_EQ(root->mode, 0755);
    return 0;
}

static int test_mkdir_creates_directory(void) {
    setup_vfs();
    int ret = vfs_mkdir("/etc", 0755);
    ASSERT_EQ(ret, 0);

    VfsStat st;
    ret = vfs_stat("/etc", &st);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(st.type, VFS_DIRECTORY);
    return 0;
}

static int test_mkdir_nested(void) {
    setup_vfs();
    ASSERT_EQ(vfs_mkdir("/a", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/a/b", 0755), 0);

    VfsStat st;
    ASSERT_EQ(vfs_stat("/a/b", &st), 0);
    ASSERT_EQ(st.type, VFS_DIRECTORY);
    return 0;
}

static int test_mkdir_no_parent_fails(void) {
    setup_vfs();
    int ret = vfs_mkdir("/a/b", 0755);
    ASSERT_EQ(ret, -ENOENT);
    return 0;
}

static int test_open_create_file(void) {
    setup_vfs();
    VfsFile f;
    int ret = vfs_open("/hello.txt", O_CREAT | O_RDWR, &f);
    ASSERT_EQ(ret, 0);
    ASSERT_TRUE(f.node != NULL);
    ASSERT_EQ(f.node->type, VFS_FILE);
    vfs_close(&f);
    return 0;
}

static int test_open_existing_file(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/hello.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_close(&f);

    /* Open again without O_CREAT */
    ASSERT_EQ(vfs_open("/hello.txt", O_RDWR, &f), 0);
    ASSERT_TRUE(f.node != NULL);
    vfs_close(&f);
    return 0;
}

static int test_open_nonexistent_fails(void) {
    setup_vfs();
    VfsFile f;
    int ret = vfs_open("/nope.txt", O_RDONLY, &f);
    ASSERT_EQ(ret, -ENOENT);
    return 0;
}

static int test_write_then_read(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/data.txt", O_CREAT | O_RDWR, &f), 0);

    const char *msg = "Hello, VFS!";
    int n = vfs_write(&f, msg, 11);
    ASSERT_EQ(n, 11);

    ASSERT_EQ(vfs_seek(&f, 0, SEEK_SET), 0);

    char buf[32];
    memset(buf, 0, sizeof(buf));
    n = vfs_read(&f, buf, sizeof(buf));
    ASSERT_EQ(n, 11);
    ASSERT_MEM_EQ(buf, "Hello, VFS!", 11);
    vfs_close(&f);
    return 0;
}

static int test_write_append(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/log.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_write(&f, "aaa", 3);
    vfs_close(&f);

    /* Re-open with O_APPEND */
    ASSERT_EQ(vfs_open("/log.txt", O_RDWR | O_APPEND, &f), 0);
    vfs_write(&f, "bbb", 3);

    vfs_seek(&f, 0, SEEK_SET);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    int n = vfs_read(&f, buf, sizeof(buf));
    ASSERT_EQ(n, 6);
    ASSERT_MEM_EQ(buf, "aaabbb", 6);
    vfs_close(&f);
    return 0;
}

static int test_write_grows_file(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/big.txt", O_CREAT | O_RDWR, &f), 0);

    /* Write a chunk bigger than RAMFS_INIT_CAP (512) */
    char big[1024];
    memset(big, 'A', sizeof(big));
    int n = vfs_write(&f, big, sizeof(big));
    ASSERT_EQ(n, 1024);
    ASSERT_EQ(f.node->size, 1024);

    vfs_seek(&f, 0, SEEK_SET);
    char readback[1024];
    n = vfs_read(&f, readback, sizeof(readback));
    ASSERT_EQ(n, 1024);
    ASSERT_MEM_EQ(readback, big, 1024);
    vfs_close(&f);
    return 0;
}

static int test_seek_set_cur_end(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/seek.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_write(&f, "0123456789", 10);

    /* SEEK_SET */
    ASSERT_EQ(vfs_seek(&f, 5, SEEK_SET), 0);
    ASSERT_EQ(f.offset, 5);

    /* SEEK_CUR */
    ASSERT_EQ(vfs_seek(&f, 2, SEEK_CUR), 0);
    ASSERT_EQ(f.offset, 7);

    /* SEEK_END */
    ASSERT_EQ(vfs_seek(&f, -3, SEEK_END), 0);
    ASSERT_EQ(f.offset, 7);

    /* Negative result should fail */
    ASSERT_EQ(vfs_seek(&f, -100, SEEK_SET), -EINVAL);

    vfs_close(&f);
    return 0;
}

static int test_readdir_lists_children(void) {
    setup_vfs();
    vfs_mkdir("/bin", 0755);
    vfs_mkdir("/etc", 0755);

    VfsFile f;
    vfs_open("/hello.txt", O_CREAT | O_RDWR, &f);
    vfs_close(&f);

    VfsDirEntry entries[16];
    int count = vfs_readdir("/", entries, 16);
    ASSERT_EQ(count, 3);

    /* Entries should be bin, etc, hello.txt (in creation order) */
    ASSERT_STR_EQ(entries[0].name, "bin");
    ASSERT_EQ(entries[0].type, VFS_DIRECTORY);
    ASSERT_STR_EQ(entries[1].name, "etc");
    ASSERT_EQ(entries[1].type, VFS_DIRECTORY);
    ASSERT_STR_EQ(entries[2].name, "hello.txt");
    ASSERT_EQ(entries[2].type, VFS_FILE);
    return 0;
}

static int test_unlink_removes_file(void) {
    setup_vfs();
    VfsFile f;
    vfs_open("/tmp.txt", O_CREAT | O_RDWR, &f);
    vfs_close(&f);

    ASSERT_EQ(vfs_unlink("/tmp.txt"), 0);

    VfsStat st;
    ASSERT_EQ(vfs_stat("/tmp.txt", &st), -ENOENT);

    VfsDirEntry entries[16];
    int count = vfs_readdir("/", entries, 16);
    ASSERT_EQ(count, 0);
    return 0;
}

static int test_unlink_nonexistent_fails(void) {
    setup_vfs();
    ASSERT_EQ(vfs_unlink("/ghost.txt"), -ENOENT);
    return 0;
}

static int test_stat_returns_info(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/info.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_write(&f, "data", 4);
    vfs_close(&f);

    VfsStat st;
    ASSERT_EQ(vfs_stat("/info.txt", &st), 0);
    ASSERT_EQ(st.type, VFS_FILE);
    ASSERT_EQ(st.size, 4);
    ASSERT_TRUE(st.inode_num > 0);
    return 0;
}

/* --- Additional edge case tests --- */

static int test_open_trunc_clears_data(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/trunc.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_write(&f, "hello", 5);
    vfs_close(&f);

    ASSERT_EQ(vfs_open("/trunc.txt", O_RDWR | O_TRUNC, &f), 0);
    ASSERT_EQ(f.node->size, 0);
    char buf[16];
    int n = vfs_read(&f, buf, sizeof(buf));
    ASSERT_EQ(n, 0);
    vfs_close(&f);
    return 0;
}

static int test_open_trunc_then_write(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/trunc2.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_write(&f, "old data", 8);
    vfs_close(&f);

    ASSERT_EQ(vfs_open("/trunc2.txt", O_RDWR | O_TRUNC, &f), 0);
    vfs_write(&f, "new", 3);
    vfs_seek(&f, 0, SEEK_SET);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    int n = vfs_read(&f, buf, sizeof(buf));
    ASSERT_EQ(n, 3);
    ASSERT_MEM_EQ(buf, "new", 3);
    vfs_close(&f);
    return 0;
}

static int test_open_creat_trunc_new_file(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/newtrunc.txt", O_CREAT | O_TRUNC | O_RDWR, &f), 0);
    ASSERT_EQ(f.node->size, 0);
    vfs_close(&f);
    return 0;
}

static int test_ramfs_max_children_boundary(void) {
    setup_vfs();
    char name[32];
    for (int i = 0; i < 128; i++) {
        sprintf(name, "/file%d.txt", i);
        VfsFile f;
        ASSERT_EQ(vfs_open(name, O_CREAT | O_RDWR, &f), 0);
        vfs_close(&f);
    }
    /* 129th should fail */
    VfsFile f;
    int ret = vfs_open("/overflow.txt", O_CREAT | O_RDWR, &f);
    ASSERT_EQ(ret, -ENOMEM);
    return 0;
}

static int test_ramfs_max_children_mkdir(void) {
    setup_vfs();
    char name[32];
    for (int i = 0; i < 128; i++) {
        sprintf(name, "/dir%d", i);
        ASSERT_EQ(vfs_mkdir(name, 0755), 0);
    }
    ASSERT_EQ(vfs_mkdir("/overflow_dir", 0755), -ENOMEM);
    return 0;
}

static int test_long_filename_max_length(void) {
    setup_vfs();
    /* 255-char filename */
    char path[258];
    path[0] = '/';
    memset(path + 1, 'a', 255);
    path[256] = '\0';

    VfsFile f;
    ASSERT_EQ(vfs_open(path, O_CREAT | O_RDWR, &f), 0);
    vfs_close(&f);

    VfsStat st;
    ASSERT_EQ(vfs_stat(path, &st), 0);
    return 0;
}

static int test_deep_nested_path(void) {
    setup_vfs();
    ASSERT_EQ(vfs_mkdir("/a", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/a/b", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/a/b/c", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/a/b/c/d", 0755), 0);

    VfsFile f;
    ASSERT_EQ(vfs_open("/a/b/c/d/deep.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_close(&f);

    VfsStat st;
    ASSERT_EQ(vfs_stat("/a/b/c/d/deep.txt", &st), 0);
    ASSERT_EQ(st.type, VFS_FILE);
    return 0;
}

static int test_open_no_creat_nonexistent(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/missing.txt", O_RDWR, &f), -ENOENT);
    return 0;
}

static int test_open_no_creat_wronly_nonexistent(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/missing.txt", O_WRONLY, &f), -ENOENT);
    return 0;
}

static int test_write_extends_file_with_seek_gap(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/gap.txt", O_CREAT | O_RDWR, &f), 0);
    /* Seek to offset 10, then write */
    vfs_seek(&f, 10, SEEK_SET);
    vfs_write(&f, "hi", 2);
    ASSERT_EQ(f.node->size, 12);

    /* Read back — bytes 0-9 should be zero */
    vfs_seek(&f, 0, SEEK_SET);
    char buf[12];
    int n = vfs_read(&f, buf, 12);
    ASSERT_EQ(n, 12);
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(buf[i], 0);
    }
    ASSERT_EQ(buf[10], 'h');
    ASSERT_EQ(buf[11], 'i');
    vfs_close(&f);
    return 0;
}

static int test_double_unlink_fails(void) {
    setup_vfs();
    VfsFile f;
    vfs_open("/once.txt", O_CREAT | O_RDWR, &f);
    vfs_close(&f);
    ASSERT_EQ(vfs_unlink("/once.txt"), 0);
    ASSERT_EQ(vfs_unlink("/once.txt"), -ENOENT);
    return 0;
}

static int test_unlink_nonempty_dir_fails(void) {
    setup_vfs();
    ASSERT_EQ(vfs_mkdir("/mydir", 0755), 0);
    VfsFile f;
    vfs_open("/mydir/child.txt", O_CREAT | O_RDWR, &f);
    vfs_close(&f);
    ASSERT_EQ(vfs_unlink("/mydir"), -ENOTEMPTY);
    return 0;
}

static int test_unlink_empty_dir_succeeds(void) {
    setup_vfs();
    ASSERT_EQ(vfs_mkdir("/emptydir", 0755), 0);
    ASSERT_EQ(vfs_unlink("/emptydir"), 0);
    VfsStat st;
    ASSERT_EQ(vfs_stat("/emptydir", &st), -ENOENT);
    return 0;
}

static int test_open_directory_fails(void) {
    setup_vfs();
    ASSERT_EQ(vfs_mkdir("/adir", 0755), 0);
    VfsFile f;
    ASSERT_EQ(vfs_open("/adir", O_RDWR, &f), -EISDIR);
    return 0;
}

static int test_mkdir_duplicate_fails(void) {
    setup_vfs();
    ASSERT_EQ(vfs_mkdir("/dup", 0755), 0);
    ASSERT_EQ(vfs_mkdir("/dup", 0755), -EEXIST);
    return 0;
}

static int test_read_past_eof_returns_zero(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/small.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_write(&f, "ab", 2);
    vfs_seek(&f, 100, SEEK_SET);
    char buf[16];
    int n = vfs_read(&f, buf, sizeof(buf));
    ASSERT_EQ(n, 0);
    vfs_close(&f);
    return 0;
}

static int test_write_rdonly_fails(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/ro.txt", O_CREAT | O_RDWR, &f), 0);
    vfs_write(&f, "data", 4);
    vfs_close(&f);

    ASSERT_EQ(vfs_open("/ro.txt", O_RDONLY, &f), 0);
    int n = vfs_write(&f, "nope", 4);
    ASSERT_EQ(n, -EINVAL);
    vfs_close(&f);
    return 0;
}

static int test_read_wronly_fails(void) {
    setup_vfs();
    VfsFile f;
    ASSERT_EQ(vfs_open("/wo.txt", O_CREAT | O_WRONLY, &f), 0);
    vfs_write(&f, "data", 4);
    vfs_seek(&f, 0, SEEK_SET);
    char buf[16];
    int n = vfs_read(&f, buf, sizeof(buf));
    ASSERT_EQ(n, -EINVAL);
    vfs_close(&f);
    return 0;
}

static int test_readdir_max_limits_output(void) {
    setup_vfs();
    VfsFile f;
    vfs_open("/a.txt", O_CREAT | O_RDWR, &f); vfs_close(&f);
    vfs_open("/b.txt", O_CREAT | O_RDWR, &f); vfs_close(&f);
    vfs_open("/c.txt", O_CREAT | O_RDWR, &f); vfs_close(&f);

    VfsDirEntry entries[2];
    int count = vfs_readdir("/", entries, 2);
    ASSERT_EQ(count, 2);
    return 0;
}

static int test_stat_directory_size_zero(void) {
    setup_vfs();
    ASSERT_EQ(vfs_mkdir("/szdir", 0755), 0);
    VfsStat st;
    ASSERT_EQ(vfs_stat("/szdir", &st), 0);
    ASSERT_EQ(st.type, VFS_DIRECTORY);
    ASSERT_EQ(st.size, 0);
    return 0;
}

/* --- Multi-mount tests --- */

static int test_mount_multiple(void) {
    setup_vfs();

    /* Create two dummy mount roots */
    VfsNode *dev_root = kmalloc(sizeof(VfsNode), GFP_ZERO);
    dev_root->type = VFS_DIRECTORY;
    dev_root->inode_num = 9001;

    VfsNode *proc_root = kmalloc(sizeof(VfsNode), GFP_ZERO);
    proc_root->type = VFS_DIRECTORY;
    proc_root->inode_num = 9002;

    ASSERT_EQ(vfs_mount("/dev", dev_root), 0);
    ASSERT_EQ(vfs_mount("/proc", proc_root), 0);

    /* Both should be resolvable */
    ASSERT_TRUE(vfs_resolve("/dev") == dev_root);
    ASSERT_TRUE(vfs_resolve("/proc") == proc_root);

    /* Root listing should include both */
    VfsDirEntry entries[16];
    int count = vfs_readdir("/", entries, 16);
    int found_dev = 0, found_proc = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, "dev") == 0) found_dev = 1;
        if (strcmp(entries[i].name, "proc") == 0) found_proc = 1;
    }
    ASSERT_TRUE(found_dev);
    ASSERT_TRUE(found_proc);

    kfree(dev_root);
    kfree(proc_root);
    return 0;
}

static int test_mount_duplicate_rejected(void) {
    setup_vfs();

    VfsNode *dev_root = kmalloc(sizeof(VfsNode), GFP_ZERO);
    dev_root->type = VFS_DIRECTORY;
    dev_root->inode_num = 9001;

    ASSERT_EQ(vfs_mount("/dev", dev_root), 0);
    ASSERT_EQ(vfs_mount("/dev", dev_root), -EEXIST);

    kfree(dev_root);
    return 0;
}

/* --- Permission integration tests --- */

static int test_kernel_context_skips_checks(void) {
    setup_vfs();
    vfs_test_proc_ptr = NULL;
    int r = vfs_mkdir("/testdir", 0000);
    ASSERT_EQ(r, 0);
    return 0;
}

static int test_open_readonly_checks_read(void) {
    setup_vfs();
    vfs_test_proc_ptr = NULL;
    VfsFile f;
    vfs_open("/testfile", O_CREAT | O_WRONLY, &f);
    vfs_close(&f);
    VfsNode *node = vfs_resolve("/testfile");
    node->mode = 0000;
    node->uid = 10;
    node->gid = 10;
    Process user_proc = { .euid = 99, .egid = 99 };
    vfs_test_proc_ptr = &user_proc;
    int r = vfs_open("/testfile", O_RDONLY, &f);
    ASSERT_EQ(r, -EACCES);
    return 0;
}

static int test_open_writable_checks_write(void) {
    setup_vfs();
    vfs_test_proc_ptr = NULL;
    VfsFile f;
    vfs_open("/testfile2", O_CREAT | O_WRONLY, &f);
    vfs_close(&f);
    VfsNode *node = vfs_resolve("/testfile2");
    node->mode = 0444;
    node->uid = 10;
    node->gid = 10;
    Process user_proc = { .euid = 99, .egid = 99 };
    vfs_test_proc_ptr = &user_proc;
    int r = vfs_open("/testfile2", O_WRONLY, &f);
    ASSERT_EQ(r, -EACCES);
    return 0;
}

static int test_mkdir_checks_parent_wx(void) {
    setup_vfs();
    vfs_test_proc_ptr = NULL;
    vfs_mkdir("/restricted", 0555);
    VfsNode *node = vfs_resolve("/restricted");
    node->uid = 10;
    node->gid = 10;
    Process user_proc = { .euid = 99, .egid = 99 };
    vfs_test_proc_ptr = &user_proc;
    int r = vfs_mkdir("/restricted/sub", 0755);
    ASSERT_EQ(r, -EACCES);
    return 0;
}

static int test_create_sets_ownership(void) {
    setup_vfs();
    Process user_proc = { .euid = 42, .egid = 7 };
    vfs_test_proc_ptr = &user_proc;
    VfsNode *root = vfs_get_root();
    root->mode = 0777;
    VfsFile f;
    int r = vfs_open("/owned_file", O_CREAT | O_WRONLY, &f);
    ASSERT_EQ(r, 0);
    vfs_close(&f);
    VfsNode *node = vfs_resolve("/owned_file");
    ASSERT_TRUE(node != NULL);
    ASSERT_EQ(node->uid, 42);
    ASSERT_EQ(node->gid, 7);
    return 0;
}

/* --- Test suite export --- */

TestCase vfs_tests[] = {
    { "init_has_root",          test_vfs_init_has_root },
    { "mkdir_creates_directory", test_mkdir_creates_directory },
    { "mkdir_nested",           test_mkdir_nested },
    { "mkdir_no_parent_fails",  test_mkdir_no_parent_fails },
    { "open_create_file",       test_open_create_file },
    { "open_existing_file",     test_open_existing_file },
    { "open_nonexistent_fails", test_open_nonexistent_fails },
    { "write_then_read",        test_write_then_read },
    { "write_append",           test_write_append },
    { "write_grows_file",       test_write_grows_file },
    { "seek_set_cur_end",       test_seek_set_cur_end },
    { "readdir_lists_children", test_readdir_lists_children },
    { "unlink_removes_file",    test_unlink_removes_file },
    { "unlink_nonexistent_fails", test_unlink_nonexistent_fails },
    { "stat_returns_info",      test_stat_returns_info },
    { "open_trunc_clears_data",        test_open_trunc_clears_data },
    { "open_trunc_then_write",         test_open_trunc_then_write },
    { "open_creat_trunc_new_file",     test_open_creat_trunc_new_file },
    { "ramfs_max_children_boundary",   test_ramfs_max_children_boundary },
    { "ramfs_max_children_mkdir",      test_ramfs_max_children_mkdir },
    { "long_filename_max_length",      test_long_filename_max_length },
    { "deep_nested_path",              test_deep_nested_path },
    { "open_no_creat_nonexistent",     test_open_no_creat_nonexistent },
    { "open_no_creat_wronly_nonexistent", test_open_no_creat_wronly_nonexistent },
    { "write_extends_file_with_seek_gap", test_write_extends_file_with_seek_gap },
    { "double_unlink_fails",           test_double_unlink_fails },
    { "unlink_nonempty_dir_fails",     test_unlink_nonempty_dir_fails },
    { "unlink_empty_dir_succeeds",     test_unlink_empty_dir_succeeds },
    { "open_directory_fails",          test_open_directory_fails },
    { "mkdir_duplicate_fails",         test_mkdir_duplicate_fails },
    { "read_past_eof_returns_zero",    test_read_past_eof_returns_zero },
    { "write_rdonly_fails",            test_write_rdonly_fails },
    { "read_wronly_fails",             test_read_wronly_fails },
    { "readdir_max_limits_output",     test_readdir_max_limits_output },
    { "stat_directory_size_zero",      test_stat_directory_size_zero },
    { "mount_multiple",                test_mount_multiple },
    { "mount_duplicate_rejected",      test_mount_duplicate_rejected },
    { "kernel_context_skips_checks",   test_kernel_context_skips_checks },
    { "open_readonly_checks_read",     test_open_readonly_checks_read },
    { "open_writable_checks_write",    test_open_writable_checks_write },
    { "mkdir_checks_parent_wx",        test_mkdir_checks_parent_wx },
    { "create_sets_ownership",         test_create_sets_ownership },
};

int vfs_test_count = sizeof(vfs_tests) / sizeof(vfs_tests[0]);
