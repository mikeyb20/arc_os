/* arc_os — Host-side tests for vfs_check_perm */

#include "test_framework.h"
#include <stdint.h>

/* Minimal VfsNode for permission tests — no vfs.c include needed */
#define VFS_FILE      0
#define VFS_DIRECTORY 1

#define EACCES 13

#define R_OK  4
#define W_OK  2
#define X_OK  1

typedef struct VfsNode {
    uint64_t       inode_num;
    uint8_t        type;
    uint64_t       size;
    uint32_t       mode;
    uint32_t       uid;
    uint32_t       gid;
    const void    *ops;
    void          *private_data;
} VfsNode;

/* Inline copy of vfs_check_perm (avoids linking vfs.c twice) */
static int vfs_check_perm(const VfsNode *node, uint32_t uid, uint32_t gid, int want) {
    if (uid == 0) return 0;

    uint32_t mode = node->mode;
    uint32_t perm;

    if (uid == node->uid) {
        perm = (mode >> 6) & 7;
    } else if (gid == node->gid) {
        perm = (mode >> 3) & 7;
    } else {
        perm = mode & 7;
    }

    if ((perm & (uint32_t)want) == (uint32_t)want) return 0;
    return -EACCES;
}

/* --- Tests --- */

TEST(root_bypasses_all) {
    VfsNode node = { .mode = 0000, .uid = 99, .gid = 99 };
    ASSERT_EQ(vfs_check_perm(&node, 0, 0, R_OK | W_OK | X_OK), 0);
    return 0;
}

TEST(owner_read_allowed) {
    VfsNode node = { .mode = 0400, .uid = 42, .gid = 1 };
    ASSERT_EQ(vfs_check_perm(&node, 42, 1, R_OK), 0);
    return 0;
}

TEST(owner_read_denied) {
    VfsNode node = { .mode = 0200, .uid = 42, .gid = 1 };
    ASSERT_EQ(vfs_check_perm(&node, 42, 1, R_OK), -EACCES);
    return 0;
}

TEST(owner_write_allowed) {
    VfsNode node = { .mode = 0200, .uid = 42, .gid = 1 };
    ASSERT_EQ(vfs_check_perm(&node, 42, 1, W_OK), 0);
    return 0;
}

TEST(owner_exec_allowed) {
    VfsNode node = { .mode = 0100, .uid = 42, .gid = 1 };
    ASSERT_EQ(vfs_check_perm(&node, 42, 1, X_OK), 0);
    return 0;
}

TEST(group_read_allowed) {
    VfsNode node = { .mode = 0040, .uid = 10, .gid = 5 };
    ASSERT_EQ(vfs_check_perm(&node, 99, 5, R_OK), 0);
    return 0;
}

TEST(group_read_denied) {
    VfsNode node = { .mode = 0400, .uid = 10, .gid = 5 };
    ASSERT_EQ(vfs_check_perm(&node, 99, 5, R_OK), -EACCES);
    return 0;
}

TEST(other_read_allowed) {
    VfsNode node = { .mode = 0004, .uid = 10, .gid = 5 };
    ASSERT_EQ(vfs_check_perm(&node, 99, 99, R_OK), 0);
    return 0;
}

TEST(other_read_denied) {
    VfsNode node = { .mode = 0640, .uid = 10, .gid = 5 };
    ASSERT_EQ(vfs_check_perm(&node, 99, 99, R_OK), -EACCES);
    return 0;
}

TEST(combined_rw_allowed) {
    VfsNode node = { .mode = 0600, .uid = 42, .gid = 1 };
    ASSERT_EQ(vfs_check_perm(&node, 42, 1, R_OK | W_OK), 0);
    return 0;
}

TEST(combined_rw_partial_denied) {
    VfsNode node = { .mode = 0400, .uid = 42, .gid = 1 };
    ASSERT_EQ(vfs_check_perm(&node, 42, 1, R_OK | W_OK), -EACCES);
    return 0;
}

/* --- Suite --- */

TestCase cred_tests[] = {
    TEST_ENTRY(root_bypasses_all),
    TEST_ENTRY(owner_read_allowed),
    TEST_ENTRY(owner_read_denied),
    TEST_ENTRY(owner_write_allowed),
    TEST_ENTRY(owner_exec_allowed),
    TEST_ENTRY(group_read_allowed),
    TEST_ENTRY(group_read_denied),
    TEST_ENTRY(other_read_allowed),
    TEST_ENTRY(other_read_denied),
    TEST_ENTRY(combined_rw_allowed),
    TEST_ENTRY(combined_rw_partial_denied),
};
int cred_test_count = sizeof(cred_tests) / sizeof(cred_tests[0]);
