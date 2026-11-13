/* arc_os — permission checks (vfs_check_perm, the real one, M1-11) */

#include "test_framework.h"
#include "vfs_env.h"

#include "uapi/errno.h"

static Inode node(uint32_t mode, uint32_t uid, uint32_t gid) {
    Inode i;
    memset(&i, 0, sizeof(i));
    i.mode = mode;
    i.uid = uid;
    i.gid = gid;
    return i;
}

TEST(root_bypasses_but_needs_an_x_bit) {
    Inode f = node(S_IFREG | 0000, 5, 5);
    ASSERT_EQ(vfs_check_perm(&f, 0, 0, R_OK | W_OK), 0);
    ASSERT_EQ(vfs_check_perm(&f, 0, 0, X_OK), -EACCES);   /* nobody may run it */
    Inode g = node(S_IFREG | 0001, 5, 5);
    ASSERT_EQ(vfs_check_perm(&g, 0, 0, X_OK), 0);
    Inode d = node(S_IFDIR | 0000, 5, 5);
    ASSERT_EQ(vfs_check_perm(&d, 0, 0, X_OK), 0);         /* directories: search always */
    return 0;
}

TEST(owner_group_other_classes) {
    Inode f = node(S_IFREG | 0640, 1000, 100);
    ASSERT_EQ(vfs_check_perm(&f, 1000, 1, R_OK | W_OK), 0);
    ASSERT_EQ(vfs_check_perm(&f, 1000, 1, X_OK), -EACCES);
    ASSERT_EQ(vfs_check_perm(&f, 2000, 100, R_OK), 0);
    ASSERT_EQ(vfs_check_perm(&f, 2000, 100, W_OK), -EACCES);
    ASSERT_EQ(vfs_check_perm(&f, 2000, 200, R_OK), -EACCES);
    /* The owner class applies even when it grants less than "other" */
    Inode o = node(S_IFREG | 0007, 1000, 100);
    ASSERT_EQ(vfs_check_perm(&o, 1000, 100, R_OK), -EACCES);
    ASSERT_EQ(vfs_check_perm(&o, 3000, 300, R_OK | W_OK | X_OK), 0);
    return 0;
}

TEST(inode_permission_uses_current_process) {
    vfs_env_setup();
    Inode f = node(S_IFREG | 0600, 1000, 1000);
    vfs_env_as(1000, 1000);
    ASSERT_EQ(inode_permission(&f, R_OK | W_OK), 0);
    vfs_env_as(1001, 1000);
    ASSERT_EQ(inode_permission(&f, R_OK), -EACCES);
    vfs_env_as(0, 0);
    return 0;
}

TestCase cred_tests[] = {
    TEST_ENTRY(root_bypasses_but_needs_an_x_bit),
    TEST_ENTRY(owner_group_other_classes),
    TEST_ENTRY(inode_permission_uses_current_process),
};
int cred_test_count = sizeof(cred_tests) / sizeof(cred_tests[0]);
