/* arc_os — Host-side tests for /etc/passwd parser */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_STRING_H

static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include passwd implementation directly */
#include "../kernel/fs/passwd.c"

/* --- Tests --- */

TEST(parse_single_user) {
    const char *buf = "root::0:0:root:/:/boot/shell\n";
    PasswdEntry entries[4];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 4);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(entries[0].name, "root");
    ASSERT_STR_EQ(entries[0].password, "");
    ASSERT_EQ(entries[0].uid, 0);
    ASSERT_EQ(entries[0].gid, 0);
    ASSERT_STR_EQ(entries[0].home, "/");
    ASSERT_STR_EQ(entries[0].shell, "/boot/shell");
    return 0;
}

TEST(parse_multiple_users) {
    const char *buf =
        "root::0:0:root:/:/boot/shell\n"
        "user:pass:1000:1000:user:/home/user:/boot/shell\n";
    PasswdEntry entries[4];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 4);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(entries[0].name, "root");
    ASSERT_EQ(entries[0].uid, 0);
    ASSERT_STR_EQ(entries[1].name, "user");
    ASSERT_STR_EQ(entries[1].password, "pass");
    ASSERT_EQ(entries[1].uid, 1000);
    ASSERT_EQ(entries[1].gid, 1000);
    ASSERT_STR_EQ(entries[1].home, "/home/user");
    return 0;
}

TEST(parse_skip_comments) {
    const char *buf =
        "# This is a comment\n"
        "root::0:0:root:/:/boot/shell\n"
        "# Another comment\n"
        "user:pw:1:1:u:/h:/s\n";
    PasswdEntry entries[4];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 4);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(entries[0].name, "root");
    ASSERT_STR_EQ(entries[1].name, "user");
    return 0;
}

TEST(parse_skip_blank_lines) {
    const char *buf =
        "\n\nroot::0:0:root:/:/s\n\n\n";
    PasswdEntry entries[4];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 4);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(entries[0].name, "root");
    return 0;
}

TEST(parse_max_entries) {
    const char *buf =
        "a::0:0:a:/a:/a\n"
        "b::1:1:b:/b:/b\n"
        "c::2:2:c:/c:/c\n";
    PasswdEntry entries[2];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 2);
    ASSERT_EQ(n, 2);  /* Should stop at max */
    ASSERT_STR_EQ(entries[0].name, "a");
    ASSERT_STR_EQ(entries[1].name, "b");
    return 0;
}

TEST(parse_null_input) {
    ASSERT_EQ(passwd_parse(NULL, 0, NULL, 0), -1);
    return 0;
}

TEST(parse_empty_buffer) {
    PasswdEntry entries[4];
    int n = passwd_parse("", 0, entries, 4);
    ASSERT_EQ(n, 0);
    return 0;
}

TEST(find_user_by_name) {
    const char *buf =
        "root::0:0:r:/:/s\n"
        "user:pw:1000:1000:u:/h:/s\n";
    PasswdEntry entries[4];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 4);
    ASSERT_EQ(n, 2);

    const PasswdEntry *found = passwd_find_user(entries, n, "user");
    ASSERT_TRUE(found != NULL);
    ASSERT_EQ(found->uid, 1000);

    const PasswdEntry *notfound = passwd_find_user(entries, n, "nobody");
    ASSERT_TRUE(notfound == NULL);
    return 0;
}

TEST(find_user_by_uid) {
    const char *buf =
        "root::0:0:r:/:/s\n"
        "user:pw:1000:1000:u:/h:/s\n";
    PasswdEntry entries[4];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 4);
    ASSERT_EQ(n, 2);

    const PasswdEntry *found = passwd_find_uid(entries, n, 0);
    ASSERT_TRUE(found != NULL);
    ASSERT_STR_EQ(found->name, "root");

    const PasswdEntry *notfound = passwd_find_uid(entries, n, 9999);
    ASSERT_TRUE(notfound == NULL);
    return 0;
}

TEST(parse_password_with_special_chars) {
    const char *buf = "test:p@ss!w0rd:500:500:t:/t:/s\n";
    PasswdEntry entries[4];
    int n = passwd_parse(buf, (uint32_t)strlen(buf), entries, 4);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(entries[0].password, "p@ss!w0rd");
    ASSERT_EQ(entries[0].uid, 500);
    return 0;
}

/* --- Suite --- */

TestCase passwd_tests[] = {
    TEST_ENTRY(parse_single_user),
    TEST_ENTRY(parse_multiple_users),
    TEST_ENTRY(parse_skip_comments),
    TEST_ENTRY(parse_skip_blank_lines),
    TEST_ENTRY(parse_max_entries),
    TEST_ENTRY(parse_null_input),
    TEST_ENTRY(parse_empty_buffer),
    TEST_ENTRY(find_user_by_name),
    TEST_ENTRY(find_user_by_uid),
    TEST_ENTRY(parse_password_with_special_chars),
};
int passwd_test_count = sizeof(passwd_tests) / sizeof(passwd_tests[0]);
