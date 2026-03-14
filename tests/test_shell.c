/* arc_os — Host-side tests for shell parsing and string utilities */

#include "test_framework.h"
#include <stdint.h>

/* --- Reproduce shell string utilities and parsing exactly as in shell.c --- */

#define MAX_ARGS 16
#define LINE_MAX 256

static uint64_t sh_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static int sh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void sh_memset(void *dst, int c, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

static int sh_atoi(const char *s) {
    int neg = 0, val = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

static int parse_line(char *line, char *argv[MAX_ARGS]) {
    for (int i = 0; line[i]; i++) {
        if (line[i] == '\n') { line[i] = '\0'; break; }
    }

    int argc = 0;
    int in_token = 0;

    for (int i = 0; line[i]; i++) {
        if (line[i] == ' ') {
            line[i] = '\0';
            in_token = 0;
        } else if (!in_token) {
            if (argc >= MAX_ARGS) break;
            argv[argc++] = &line[i];
            in_token = 1;
        }
    }
    return argc;
}

/* Suppress unused warnings for stubs reproduced from shell.c */
static void suppress_unused(void) {
    (void)sh_memset;
}

/* --- Tests --- */

static int test_parse_empty(void) {
    suppress_unused();
    char line[] = "";
    char *argv[MAX_ARGS];
    ASSERT_EQ(parse_line(line, argv), 0);
    return 0;
}

static int test_parse_newline_only(void) {
    char line[] = "\n";
    char *argv[MAX_ARGS];
    ASSERT_EQ(parse_line(line, argv), 0);
    return 0;
}

static int test_parse_spaces_only(void) {
    char line[] = "   \n";
    char *argv[MAX_ARGS];
    ASSERT_EQ(parse_line(line, argv), 0);
    return 0;
}

static int test_parse_single_word(void) {
    char line[] = "help\n";
    char *argv[MAX_ARGS];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "help");
    return 0;
}

static int test_parse_two_words(void) {
    char line[] = "ls /boot\n";
    char *argv[MAX_ARGS];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[0], "ls");
    ASSERT_STR_EQ(argv[1], "/boot");
    return 0;
}

static int test_parse_multiple_spaces(void) {
    char line[] = "echo   hello   world\n";
    char *argv[MAX_ARGS];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 3);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "hello");
    ASSERT_STR_EQ(argv[2], "world");
    return 0;
}

static int test_parse_leading_spaces(void) {
    char line[] = "  ls\n";
    char *argv[MAX_ARGS];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "ls");
    return 0;
}

static int test_parse_trailing_spaces(void) {
    char line[] = "ls  \n";
    char *argv[MAX_ARGS];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "ls");
    return 0;
}

static int test_parse_max_args(void) {
    char line[] = "a b c d e f g h i j k l m n o p q\n";
    char *argv[MAX_ARGS];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, MAX_ARGS);  /* Capped at 16 */
    ASSERT_STR_EQ(argv[0], "a");
    ASSERT_STR_EQ(argv[15], "p");
    return 0;
}

static int test_parse_no_newline(void) {
    char line[] = "help";
    char *argv[MAX_ARGS];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "help");
    return 0;
}

static int test_strcmp_equal(void) {
    ASSERT_EQ(sh_strcmp("abc", "abc"), 0);
    return 0;
}

static int test_strcmp_less(void) {
    ASSERT_TRUE(sh_strcmp("abc", "abd") < 0);
    return 0;
}

static int test_strcmp_greater(void) {
    ASSERT_TRUE(sh_strcmp("abd", "abc") > 0);
    return 0;
}

static int test_atoi_positive(void) {
    ASSERT_EQ(sh_atoi("42"), 42);
    return 0;
}

static int test_atoi_zero(void) {
    ASSERT_EQ(sh_atoi("0"), 0);
    return 0;
}

static int test_atoi_negative(void) {
    ASSERT_EQ(sh_atoi("-1"), -1);
    return 0;
}

static int test_atoi_non_numeric(void) {
    ASSERT_EQ(sh_atoi("abc"), 0);
    return 0;
}

static int test_strlen_basic(void) {
    ASSERT_EQ(sh_strlen("hello"), 5);
    return 0;
}

static int test_strlen_empty(void) {
    ASSERT_EQ(sh_strlen(""), 0);
    return 0;
}

/* --- Test suite export --- */

TestCase shell_tests[] = {
    { "parse_empty",          test_parse_empty },
    { "parse_newline_only",   test_parse_newline_only },
    { "parse_spaces_only",    test_parse_spaces_only },
    { "parse_single_word",    test_parse_single_word },
    { "parse_two_words",      test_parse_two_words },
    { "parse_multiple_spaces",test_parse_multiple_spaces },
    { "parse_leading_spaces", test_parse_leading_spaces },
    { "parse_trailing_spaces",test_parse_trailing_spaces },
    { "parse_max_args",       test_parse_max_args },
    { "parse_no_newline",     test_parse_no_newline },
    { "strcmp_equal",          test_strcmp_equal },
    { "strcmp_less",           test_strcmp_less },
    { "strcmp_greater",        test_strcmp_greater },
    { "atoi_positive",        test_atoi_positive },
    { "atoi_zero",            test_atoi_zero },
    { "atoi_negative",        test_atoi_negative },
    { "atoi_non_numeric",     test_atoi_non_numeric },
    { "strlen_basic",         test_strlen_basic },
    { "strlen_empty",         test_strlen_empty },
};

int shell_test_count = sizeof(shell_tests) / sizeof(shell_tests[0]);
