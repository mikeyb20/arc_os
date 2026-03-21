/* arc_os — Host-side tests for path normalization */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers that would conflict with system headers */
#define ARCHOS_LIB_STRING_H

/* Provide strlen/strncpy that path.c needs (use libc versions) */

/* Include path.c directly */
#include "../kernel/fs/path.c"

/* Error codes (match vfs.h) */
#ifndef EINVAL
#define EINVAL       22
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

/* --- Tests --- */

TEST(relative_simple) {
    char out[512];
    ASSERT_EQ(path_normalize("/", "etc", out, 512), 0);
    ASSERT_STR_EQ(out, "/etc");
    return 0;
}

TEST(relative_nested) {
    char out[512];
    ASSERT_EQ(path_normalize("/home", "user", out, 512), 0);
    ASSERT_STR_EQ(out, "/home/user");
    return 0;
}

TEST(absolute_overrides_cwd) {
    char out[512];
    ASSERT_EQ(path_normalize("/home", "/absolute", out, 512), 0);
    ASSERT_STR_EQ(out, "/absolute");
    return 0;
}

TEST(dotdot_single) {
    char out[512];
    ASSERT_EQ(path_normalize("/a/b/c", "..", out, 512), 0);
    ASSERT_STR_EQ(out, "/a/b");
    return 0;
}

TEST(dotdot_double) {
    char out[512];
    ASSERT_EQ(path_normalize("/a/b/c", "../..", out, 512), 0);
    ASSERT_STR_EQ(out, "/a");
    return 0;
}

TEST(dotdot_past_root) {
    char out[512];
    ASSERT_EQ(path_normalize("/a", "../../..", out, 512), 0);
    ASSERT_STR_EQ(out, "/");
    return 0;
}

TEST(dot_component) {
    char out[512];
    ASSERT_EQ(path_normalize("/a/b", "./c", out, 512), 0);
    ASSERT_STR_EQ(out, "/a/b/c");
    return 0;
}

TEST(collapsed_slashes) {
    char out[512];
    ASSERT_EQ(path_normalize("/", "a//b///c", out, 512), 0);
    ASSERT_STR_EQ(out, "/a/b/c");
    return 0;
}

TEST(trailing_slash) {
    char out[512];
    ASSERT_EQ(path_normalize("/", "a/b/c/", out, 512), 0);
    ASSERT_STR_EQ(out, "/a/b/c");
    return 0;
}

TEST(dot_at_root) {
    char out[512];
    ASSERT_EQ(path_normalize("/", ".", out, 512), 0);
    ASSERT_STR_EQ(out, "/");
    return 0;
}

TEST(dotdot_at_root) {
    char out[512];
    ASSERT_EQ(path_normalize("/", "..", out, 512), 0);
    ASSERT_STR_EQ(out, "/");
    return 0;
}

TEST(mid_path_dotdot) {
    char out[512];
    ASSERT_EQ(path_normalize("/a/b", "c/d/../e", out, 512), 0);
    ASSERT_STR_EQ(out, "/a/b/c/e");
    return 0;
}

TEST(too_long) {
    /* Build a very long path that exceeds a small buffer */
    char out[8];
    ASSERT_EQ(path_normalize("/", "a/b/c/d/e/f", out, 8), -ENAMETOOLONG);
    return 0;
}

TEST(null_input) {
    char out[512];
    ASSERT_EQ(path_normalize("/", NULL, out, 512), -EINVAL);
    return 0;
}

TEST(empty_input) {
    char out[512];
    /* Empty string = stay at cwd */
    ASSERT_EQ(path_normalize("/home", "", out, 512), 0);
    ASSERT_STR_EQ(out, "/home");
    return 0;
}

TEST(mount_boundary_dotdot) {
    /* /dev/.. should normalize to / (pure string, no FS knowledge needed) */
    char out[512];
    ASSERT_EQ(path_normalize("/", "/dev/..", out, 512), 0);
    ASSERT_STR_EQ(out, "/");
    return 0;
}

/* --- Suite registration --- */

TestCase path_tests[] = {
    TEST_ENTRY(relative_simple),
    TEST_ENTRY(relative_nested),
    TEST_ENTRY(absolute_overrides_cwd),
    TEST_ENTRY(dotdot_single),
    TEST_ENTRY(dotdot_double),
    TEST_ENTRY(dotdot_past_root),
    TEST_ENTRY(dot_component),
    TEST_ENTRY(collapsed_slashes),
    TEST_ENTRY(trailing_slash),
    TEST_ENTRY(dot_at_root),
    TEST_ENTRY(dotdot_at_root),
    TEST_ENTRY(mid_path_dotdot),
    TEST_ENTRY(too_long),
    TEST_ENTRY(null_input),
    TEST_ENTRY(empty_input),
    TEST_ENTRY(mount_boundary_dotdot),
};
int path_test_count = sizeof(path_tests) / sizeof(path_tests[0]);
