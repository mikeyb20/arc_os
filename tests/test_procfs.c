/* arc_os — Host-side tests for procfs */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard all kernel headers that procfs.c includes */
#define ARCHOS_FS_VFS_H
#define ARCHOS_FS_PROCFS_H
#define ARCHOS_MM_PMM_H
#define ARCHOS_BOOT_BOOTINFO_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_ARCH_X86_64_PIT_H
#define ARCHOS_PROC_PROCESS_H
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_SIGNAL_H
#define ARCHOS_PROC_WAITQUEUE_H
#define ARCHOS_PROC_SPINLOCK_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_STRING_H

/* VFS types (inline — match vfs.h) */
#define VFS_FILE      0
#define VFS_DIRECTORY 1
#define VFS_NAME_MAX  256

typedef struct VfsNode VfsNode;
typedef struct VfsDirEntry VfsDirEntry;

typedef struct {
    int      (*read)(VfsNode *node, void *buf, uint32_t offset, uint32_t size);
    int      (*write)(VfsNode *node, const void *buf, uint32_t offset, uint32_t size);
    VfsNode *(*lookup)(VfsNode *dir, const char *name);
    VfsNode *(*create)(VfsNode *dir, const char *name, uint8_t type);
    int      (*unlink)(VfsNode *dir, const char *name);
    int      (*readdir)(VfsNode *dir, VfsDirEntry *entries, uint32_t max);
    void     (*truncate)(VfsNode *node, uint64_t size);
} VfsOps;

struct VfsNode {
    uint64_t       inode_num;
    uint8_t        type;
    uint64_t       size;
    uint32_t       mode;
    const VfsOps  *ops;
    void          *private_data;
};

struct VfsDirEntry {
    char     name[VFS_NAME_MAX];
    uint64_t inode_num;
    uint8_t  type;
};

/* PMM constants */
#define PAGE_SIZE 4096

/* HeapStats type */
typedef struct {
    size_t total_used;
    size_t total_free;
    size_t total_blocks;
    size_t free_blocks;
    size_t largest_free;
    size_t heap_mapped;
} HeapStats;

/* Process states */
#define PROC_ALIVE       0
#define PROC_ZOMBIE      1
#define PROC_TERMINATED  2

/* Minimal Process struct — only fields procfs accesses */
typedef struct Process {
    uint32_t        pid;
    uint8_t         state;
    struct Process *parent;
    struct Process *next;
} Process;

/* --- Stub control values --- */

static uint64_t stub_total_pages = 32768;   /* 128 MB */
static uint64_t stub_free_pages = 16384;    /* 64 MB */
static uint64_t stub_uptime_ms = 12345;
static HeapStats stub_heap_stats = {
    .total_used = 4096,
    .total_free = 12288,
    .total_blocks = 5,
    .free_blocks = 2,
    .largest_free = 8192,
    .heap_mapped = 16384,
};

/* Test processes */
static Process test_procs[4];
static int test_proc_count = 0;

static void setup_test_procs(void) {
    memset(test_procs, 0, sizeof(test_procs));
    test_procs[0].pid = 0;
    test_procs[0].state = PROC_ALIVE;
    test_procs[0].parent = NULL;

    test_procs[1].pid = 1;
    test_procs[1].state = PROC_ALIVE;
    test_procs[1].parent = &test_procs[0];

    test_procs[2].pid = 2;
    test_procs[2].state = PROC_ZOMBIE;
    test_procs[2].parent = &test_procs[0];

    test_procs[3].pid = 99;
    test_procs[3].state = PROC_TERMINATED;
    test_procs[3].parent = NULL;

    test_proc_count = 4;
}

/* --- Kernel function stubs --- */

static uint64_t pmm_get_total_pages(void) { return stub_total_pages; }
static uint64_t pmm_get_free_pages(void) { return stub_free_pages; }
static uint64_t pit_get_uptime_ms(void) { return stub_uptime_ms; }

static void kmalloc_get_stats(HeapStats *out) {
    *out = stub_heap_stats;
}

static Process *proc_get_by_pid(uint32_t pid) {
    for (int i = 0; i < test_proc_count; i++) {
        if (test_procs[i].pid == pid && test_procs[i].state != PROC_TERMINATED) {
            return &test_procs[i];
        }
    }
    return NULL;
}

static int proc_foreach(void (*cb)(Process *p, void *ctx), void *ctx) {
    int count = 0;
    for (int i = 0; i < test_proc_count; i++) {
        if (test_procs[i].state != PROC_TERMINATED) {
            cb(&test_procs[i], ctx);
            count++;
        }
    }
    return count;
}

/* Declare procfs_init before including the .c */
VfsNode *procfs_init(void);

#include "../kernel/fs/procfs.c"

/* --- Tests --- */

TEST(init_returns_non_null) {
    VfsNode *root = procfs_init();
    ASSERT_TRUE(root != NULL);
    return 0;
}

TEST(root_is_directory) {
    VfsNode *root = procfs_init();
    ASSERT_EQ(root->type, VFS_DIRECTORY);
    return 0;
}

TEST(lookup_meminfo) {
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "meminfo");
    ASSERT_TRUE(n != NULL);
    ASSERT_EQ(n->type, VFS_FILE);
    return 0;
}

TEST(lookup_uptime) {
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "uptime");
    ASSERT_TRUE(n != NULL);
    ASSERT_EQ(n->type, VFS_FILE);
    return 0;
}

TEST(lookup_pid) {
    setup_test_procs();
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "1");
    ASSERT_TRUE(n != NULL);
    ASSERT_EQ(n->type, VFS_DIRECTORY);
    return 0;
}

TEST(lookup_nonexistent_pid) {
    setup_test_procs();
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "999");
    ASSERT_TRUE(n == NULL);
    return 0;
}

TEST(lookup_non_numeric) {
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "blah");
    ASSERT_TRUE(n == NULL);
    return 0;
}

TEST(meminfo_content) {
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "meminfo");

    char buf[512] = {0};
    int rd = n->ops->read(n, buf, 0, sizeof(buf) - 1);
    ASSERT_TRUE(rd > 0);
    ASSERT_TRUE(strstr(buf, "MemTotal:") != NULL);
    ASSERT_TRUE(strstr(buf, "MemFree:") != NULL);
    ASSERT_TRUE(strstr(buf, "HeapUsed:") != NULL);
    return 0;
}

TEST(meminfo_partial_read) {
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "meminfo");

    /* Read only first 10 bytes */
    char buf[16] = {0};
    int rd = n->ops->read(n, buf, 0, 10);
    ASSERT_EQ(rd, 10);
    ASSERT_MEM_EQ(buf, "MemTotal: ", 10);

    /* Read with offset past content */
    char buf2[16] = {0};
    rd = n->ops->read(n, buf2, 9999, 10);
    ASSERT_EQ(rd, 0);
    return 0;
}

TEST(uptime_content) {
    stub_uptime_ms = 12345;
    VfsNode *root = procfs_init();
    VfsNode *n = root->ops->lookup(root, "uptime");

    char buf[64] = {0};
    int rd = n->ops->read(n, buf, 0, sizeof(buf) - 1);
    ASSERT_TRUE(rd > 0);
    ASSERT_STR_EQ(buf, "12.345\n");
    return 0;
}

TEST(pid_status_content) {
    setup_test_procs();
    VfsNode *root = procfs_init();
    VfsNode *pid_dir = root->ops->lookup(root, "1");
    ASSERT_TRUE(pid_dir != NULL);

    VfsNode *status = pid_dir->ops->lookup(pid_dir, "status");
    ASSERT_TRUE(status != NULL);

    char buf[256] = {0};
    int rd = status->ops->read(status, buf, 0, sizeof(buf) - 1);
    ASSERT_TRUE(rd > 0);
    ASSERT_TRUE(strstr(buf, "Pid: 1") != NULL);
    ASSERT_TRUE(strstr(buf, "State: running") != NULL);
    ASSERT_TRUE(strstr(buf, "PPid: 0") != NULL);
    return 0;
}

TEST(readdir_root) {
    setup_test_procs();
    VfsNode *root = procfs_init();
    VfsDirEntry entries[16];
    int count = root->ops->readdir(root, entries, 16);
    /* meminfo + uptime + 3 non-terminated PIDs (0, 1, 2) */
    ASSERT_EQ(count, 5);
    ASSERT_STR_EQ(entries[0].name, "meminfo");
    ASSERT_STR_EQ(entries[1].name, "uptime");
    return 0;
}

TEST(readdir_pid_dir) {
    setup_test_procs();
    VfsNode *root = procfs_init();
    VfsNode *pid_dir = root->ops->lookup(root, "0");
    ASSERT_TRUE(pid_dir != NULL);

    VfsDirEntry entries[4];
    int count = pid_dir->ops->readdir(pid_dir, entries, 4);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(entries[0].name, "status");
    return 0;
}

TEST(no_create_ops) {
    VfsNode *root = procfs_init();
    ASSERT_TRUE(root->ops->create == NULL);
    ASSERT_TRUE(root->ops->unlink == NULL);
    return 0;
}

/* --- Suite --- */

TestCase procfs_tests[] = {
    TEST_ENTRY(init_returns_non_null),
    TEST_ENTRY(root_is_directory),
    TEST_ENTRY(lookup_meminfo),
    TEST_ENTRY(lookup_uptime),
    TEST_ENTRY(lookup_pid),
    TEST_ENTRY(lookup_nonexistent_pid),
    TEST_ENTRY(lookup_non_numeric),
    TEST_ENTRY(meminfo_content),
    TEST_ENTRY(meminfo_partial_read),
    TEST_ENTRY(uptime_content),
    TEST_ENTRY(pid_status_content),
    TEST_ENTRY(readdir_root),
    TEST_ENTRY(readdir_pid_dir),
    TEST_ENTRY(no_create_ops),
};
int procfs_test_count = sizeof(procfs_tests) / sizeof(procfs_tests[0]);
