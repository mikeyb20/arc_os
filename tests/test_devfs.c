/* arc_os — Host-side tests for devfs */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers */
#define ARCHOS_FS_VFS_H
#define ARCHOS_FS_DEVFS_H
#define ARCHOS_DRIVERS_TTY_H
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
    uint32_t       uid;
    uint32_t       gid;
    const VfsOps  *ops;
    void          *private_data;
};

struct VfsDirEntry {
    char     name[VFS_NAME_MAX];
    uint64_t inode_num;
    uint8_t  type;
};

/* --- TTY stubs with capture buffers --- */

static char tty_write_capture[256];
static int tty_write_capture_len;
static char tty_read_data[256];
static int tty_read_data_len;
static int tty_read_data_pos;

static int tty_read(void *buf, uint32_t count) {
    int avail = tty_read_data_len - tty_read_data_pos;
    int to_read = (int)count < avail ? (int)count : avail;
    memcpy(buf, tty_read_data + tty_read_data_pos, to_read);
    tty_read_data_pos += to_read;
    return to_read;
}

static int tty_write(const void *buf, uint32_t count) {
    int to_copy = (int)count;
    if (tty_write_capture_len + to_copy > (int)sizeof(tty_write_capture))
        to_copy = (int)sizeof(tty_write_capture) - tty_write_capture_len;
    memcpy(tty_write_capture + tty_write_capture_len, buf, to_copy);
    tty_write_capture_len += to_copy;
    return (int)count;
}

static void tty_reset(void) {
    tty_write_capture_len = 0;
    memset(tty_write_capture, 0, sizeof(tty_write_capture));
    tty_read_data_len = 0;
    tty_read_data_pos = 0;
    memset(tty_read_data, 0, sizeof(tty_read_data));
}

/* Declare devfs_init before including the .c */
VfsNode *devfs_init(void);

#include "../kernel/fs/devfs.c"

/* --- Tests --- */

TEST(init_returns_non_null) {
    VfsNode *root = devfs_init();
    ASSERT_TRUE(root != NULL);
    return 0;
}

TEST(root_is_directory) {
    VfsNode *root = devfs_init();
    ASSERT_EQ(root->type, VFS_DIRECTORY);
    return 0;
}

TEST(lookup_null) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "null");
    ASSERT_TRUE(n != NULL);
    ASSERT_EQ(n->type, VFS_FILE);
    return 0;
}

TEST(lookup_zero) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "zero");
    ASSERT_TRUE(n != NULL);
    ASSERT_EQ(n->type, VFS_FILE);
    return 0;
}

TEST(lookup_tty) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "tty");
    ASSERT_TRUE(n != NULL);
    ASSERT_EQ(n->type, VFS_FILE);
    return 0;
}

TEST(lookup_nonexistent) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "nonexistent");
    ASSERT_TRUE(n == NULL);
    return 0;
}

TEST(null_read_returns_eof) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "null");
    char buf[16];
    int rd = n->ops->read(n, buf, 0, 16);
    ASSERT_EQ(rd, 0);
    return 0;
}

TEST(null_write_discards) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "null");
    int wr = n->ops->write(n, "hello", 0, 5);
    ASSERT_EQ(wr, 5);
    return 0;
}

TEST(zero_read_returns_zeros) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "zero");
    uint8_t buf[32];
    memset(buf, 0xFF, sizeof(buf));
    int rd = n->ops->read(n, buf, 0, 32);
    ASSERT_EQ(rd, 32);
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(buf[i], 0);
    }
    return 0;
}

TEST(zero_write_discards) {
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "zero");
    int wr = n->ops->write(n, "data", 0, 4);
    ASSERT_EQ(wr, 4);
    return 0;
}

TEST(tty_read_calls_through) {
    tty_reset();
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "tty");
    memcpy(tty_read_data, "abc", 3);
    tty_read_data_len = 3;

    char buf[16] = {0};
    int rd = n->ops->read(n, buf, 0, 3);
    ASSERT_EQ(rd, 3);
    ASSERT_MEM_EQ(buf, "abc", 3);
    return 0;
}

TEST(tty_write_calls_through) {
    tty_reset();
    VfsNode *root = devfs_init();
    VfsNode *n = root->ops->lookup(root, "tty");

    int wr = n->ops->write(n, "hello", 0, 5);
    ASSERT_EQ(wr, 5);
    ASSERT_EQ(tty_write_capture_len, 5);
    ASSERT_MEM_EQ(tty_write_capture, "hello", 5);
    return 0;
}

TEST(readdir_lists_three) {
    VfsNode *root = devfs_init();
    VfsDirEntry entries[8];
    int count = root->ops->readdir(root, entries, 8);
    ASSERT_EQ(count, 3);
    ASSERT_STR_EQ(entries[0].name, "null");
    ASSERT_STR_EQ(entries[1].name, "zero");
    ASSERT_STR_EQ(entries[2].name, "tty");
    return 0;
}

TEST(no_create_or_unlink) {
    VfsNode *root = devfs_init();
    ASSERT_TRUE(root->ops->create == NULL);
    ASSERT_TRUE(root->ops->unlink == NULL);
    return 0;
}

/* --- Suite --- */

TestCase devfs_tests[] = {
    TEST_ENTRY(init_returns_non_null),
    TEST_ENTRY(root_is_directory),
    TEST_ENTRY(lookup_null),
    TEST_ENTRY(lookup_zero),
    TEST_ENTRY(lookup_tty),
    TEST_ENTRY(lookup_nonexistent),
    TEST_ENTRY(null_read_returns_eof),
    TEST_ENTRY(null_write_discards),
    TEST_ENTRY(zero_read_returns_zeros),
    TEST_ENTRY(zero_write_discards),
    TEST_ENTRY(tty_read_calls_through),
    TEST_ENTRY(tty_write_calls_through),
    TEST_ENTRY(readdir_lists_three),
    TEST_ENTRY(no_create_or_unlink),
};
int devfs_test_count = sizeof(devfs_tests) / sizeof(devfs_tests[0]);
