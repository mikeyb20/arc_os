/* arc_os — Host-side tests for shell parsing, variables, quoting, and expansion */

#include "test_framework.h"
#include <stdint.h>

/* --- Reproduce shell string utilities exactly as in shell.c --- */

#define MAX_ARGS 16
#define MAX_ARGV (MAX_ARGS + 1)
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

static uint64_t sh_strlcpy(char *dst, const char *src, uint64_t size) {
    uint64_t slen = sh_strlen(src);
    if (size > 0) {
        uint64_t copy = slen < size - 1 ? slen : size - 1;
        for (uint64_t i = 0; i < copy; i++) dst[i] = src[i];
        dst[copy] = '\0';
    }
    return slen;
}

static char *sh_strchr(const char *s, int c) {
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (void *)0;
}

static int sh_strncmp(const char *a, const char *b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

static void sh_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

/* --- Shell variable storage (reproduced from shell.c) --- */

#define MAX_VARS      32
#define VAR_NAME_MAX  32
#define VAR_VALUE_MAX 128

static char var_names[MAX_VARS][VAR_NAME_MAX];
static char var_values[MAX_VARS][VAR_VALUE_MAX];
static int  var_count = 0;

static void vars_reset(void) {
    var_count = 0;
    sh_memset(var_names, 0, sizeof(var_names));
    sh_memset(var_values, 0, sizeof(var_values));
}

static int shell_findvar(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (sh_strcmp(var_names[i], name) == 0) return i;
    return -1;
}

static const char *shell_getvar(const char *name) {
    int i = shell_findvar(name);
    return i >= 0 ? var_values[i] : (void *)0;
}

static int shell_setvar(const char *name, const char *value) {
    if (sh_strlen(name) >= VAR_NAME_MAX || sh_strlen(value) >= VAR_VALUE_MAX)
        return -1;
    int i = shell_findvar(name);
    if (i >= 0) {
        sh_strlcpy(var_values[i], value, VAR_VALUE_MAX);
        return 0;
    }
    if (var_count >= MAX_VARS) return -1;
    sh_strlcpy(var_names[var_count], name, VAR_NAME_MAX);
    sh_strlcpy(var_values[var_count], value, VAR_VALUE_MAX);
    var_count++;
    return 0;
}

static int shell_unsetvar(const char *name) {
    int i = shell_findvar(name);
    if (i < 0) return -1;
    var_count--;
    if (i < var_count) {
        sh_memcpy(var_names[i], var_names[var_count], VAR_NAME_MAX);
        sh_memcpy(var_values[i], var_values[var_count], VAR_VALUE_MAX);
    }
    return 0;
}

/* --- Variable expansion helpers (reproduced from shell.c) --- */

static int is_var_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_var_char(char c) {
    return is_var_start(c) || (c >= '0' && c <= '9');
}

static int expand_variables(const char *input, char *output, uint64_t output_size) {
    uint64_t wi = 0;
    int in_single = 0, in_double = 0;

    for (uint64_t ri = 0; input[ri]; ri++) {
        char ch = input[ri];

        if (ch == '\'' && !in_double) {
            in_single = !in_single;
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }
        if (ch == '"' && !in_single) {
            in_double = !in_double;
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }
        if (in_single) {
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }
        if (ch == '\\') {
            if (in_double) {
                char next = input[ri + 1];
                if (next == '$' || next == '"' || next == '\\') {
                    if (wi >= output_size - 1) return -1;
                    output[wi++] = next;
                    ri++;
                    continue;
                }
            } else {
                if (input[ri + 1] == '$') {
                    if (wi >= output_size - 1) return -1;
                    output[wi++] = '$';
                    ri++;
                    continue;
                }
            }
            if (wi >= output_size - 1) return -1;
            output[wi++] = ch;
            continue;
        }
        if (ch == '$') {
            if (input[ri + 1] == '$') {
                if (wi >= output_size - 1) return -1;
                output[wi++] = '$';
                ri++;
                continue;
            }
            if (is_var_start(input[ri + 1])) {
                uint64_t start = ri + 1;
                uint64_t end = start;
                while (is_var_char(input[end])) end++;

                char name[VAR_NAME_MAX];
                uint64_t nlen = end - start;
                if (nlen >= VAR_NAME_MAX) nlen = VAR_NAME_MAX - 1;
                for (uint64_t j = 0; j < nlen; j++) name[j] = input[start + j];
                name[nlen] = '\0';

                const char *val = shell_getvar(name);
                if (val) {
                    uint64_t vlen = sh_strlen(val);
                    if (wi + vlen >= output_size) return -1;
                    for (uint64_t j = 0; j < vlen; j++) output[wi++] = val[j];
                }
                ri = end - 1;
                continue;
            }
            if (wi >= output_size - 1) return -1;
            output[wi++] = '$';
            continue;
        }
        if (wi >= output_size - 1) return -1;
        output[wi++] = ch;
    }

    output[wi] = '\0';
    return 0;
}

/* --- Quote-aware parse_line (reproduced from shell.c) --- */

static int parse_line(char *line, char *argv[MAX_ARGV]) {
    for (int i = 0; line[i]; i++) {
        if (line[i] == '\n') { line[i] = '\0'; break; }
    }

    int argc = 0;
    int in_token = 0;
    int wi = 0;

    for (int ri = 0; line[ri]; ri++) {
        char ch = line[ri];

        if (!in_token) {
            if (ch == ' ') continue;
            if (argc >= MAX_ARGS) break;
            argv[argc++] = &line[wi];
            in_token = 1;
        }

        if (ch == ' ') {
            line[wi++] = '\0';
            in_token = 0;
            continue;
        }
        if (ch == '\'') {
            ri++;
            while (line[ri] && line[ri] != '\'') {
                line[wi++] = line[ri++];
            }
            if (!line[ri]) ri--;
            continue;
        }
        if (ch == '"') {
            ri++;
            while (line[ri] && line[ri] != '"') {
                if (line[ri] == '\\' && line[ri + 1]) {
                    char next = line[ri + 1];
                    if (next == '"' || next == '\\' || next == '$') {
                        line[wi++] = next;
                        ri += 2;
                        continue;
                    }
                }
                line[wi++] = line[ri++];
            }
            if (!line[ri]) ri--;
            continue;
        }
        if (ch == '\\' && line[ri + 1]) {
            ri++;
            line[wi++] = line[ri];
            continue;
        }
        line[wi++] = ch;
    }

    if (in_token) {
        line[wi] = '\0';
    }

    argv[argc] = (void *)0;
    return argc;
}

/* --- Quote-aware find_pipe (reproduced from shell.c) --- */

static char *find_pipe(char *line) {
    int in_single = 0, in_double = 0;
    for (int i = 0; line[i]; i++) {
        char ch = line[i];
        if (ch == '\\' && line[i + 1] && !in_single) { i++; continue; }
        if (ch == '\'' && !in_double) { in_single = !in_single; continue; }
        if (ch == '"' && !in_single) { in_double = !in_double; continue; }
        if (ch == '|' && !in_single && !in_double) return &line[i];
    }
    return (void *)0;
}

/* --- try_assignment (reproduced from shell.c) --- */

static int try_assignment(const char *arg) {
    if (!is_var_start(arg[0])) return 0;
    int i = 1;
    while (is_var_char(arg[i])) i++;
    return arg[i] == '=';
}

/* --- Reproduce Redirect struct and parse_redirects from shell.c --- */

typedef struct {
    const char *in_file;
    const char *out_file;
    int         append;
} Redirect;

/* Stub print for parse_redirects error messages */
static void print(const char *s) { (void)s; }

static int parse_redirects(int *argc, char *argv[], Redirect *r) {
    r->in_file = (void *)0;
    r->out_file = (void *)0;
    r->append = 0;

    int out = 0;
    int i = 0;
    while (i < *argc) {
        char *tok = argv[i];

        if (tok[0] == '>' && tok[1] == '>') {
            if (tok[2] != '\0') {
                r->out_file = &tok[2];
            } else {
                i++;
                if (i >= *argc) {
                    print("syntax error: missing filename after >>\n");
                    return -1;
                }
                r->out_file = argv[i];
            }
            r->append = 1;
            i++;
        } else if (tok[0] == '>') {
            if (tok[1] != '\0') {
                r->out_file = &tok[1];
            } else {
                i++;
                if (i >= *argc) {
                    print("syntax error: missing filename after >\n");
                    return -1;
                }
                r->out_file = argv[i];
            }
            r->append = 0;
            i++;
        } else if (tok[0] == '<') {
            if (tok[1] != '\0') {
                r->in_file = &tok[1];
            } else {
                i++;
                if (i >= *argc) {
                    print("syntax error: missing filename after <\n");
                    return -1;
                }
                r->in_file = argv[i];
            }
            i++;
        } else {
            argv[out++] = argv[i++];
        }
    }
    *argc = out;
    return 0;
}

/* Suppress unused warnings for stubs reproduced from shell.c */
static void suppress_unused(void) {
    (void)sh_memset;
    (void)sh_strncmp;
}

/* =========================================================================
 * EXISTING TESTS (backward compatible — all 33 must still pass)
 * ========================================================================= */

static int test_parse_empty(void) {
    suppress_unused();
    char line[] = "";
    char *argv[MAX_ARGV];
    ASSERT_EQ(parse_line(line, argv), 0);
    return 0;
}

static int test_parse_newline_only(void) {
    char line[] = "\n";
    char *argv[MAX_ARGV];
    ASSERT_EQ(parse_line(line, argv), 0);
    return 0;
}

static int test_parse_spaces_only(void) {
    char line[] = "   \n";
    char *argv[MAX_ARGV];
    ASSERT_EQ(parse_line(line, argv), 0);
    return 0;
}

static int test_parse_single_word(void) {
    char line[] = "help\n";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "help");
    return 0;
}

static int test_parse_two_words(void) {
    char line[] = "ls /boot\n";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[0], "ls");
    ASSERT_STR_EQ(argv[1], "/boot");
    return 0;
}

static int test_parse_multiple_spaces(void) {
    char line[] = "echo   hello   world\n";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 3);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "hello");
    ASSERT_STR_EQ(argv[2], "world");
    return 0;
}

static int test_parse_leading_spaces(void) {
    char line[] = "  ls\n";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "ls");
    return 0;
}

static int test_parse_trailing_spaces(void) {
    char line[] = "ls  \n";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "ls");
    return 0;
}

static int test_parse_max_args(void) {
    char line[] = "a b c d e f g h i j k l m n o p q\n";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, MAX_ARGS);  /* Capped at 16 */
    ASSERT_STR_EQ(argv[0], "a");
    ASSERT_STR_EQ(argv[15], "p");
    return 0;
}

static int test_parse_no_newline(void) {
    char line[] = "help";
    char *argv[MAX_ARGV];
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

/* --- Redirect tests --- */

static int test_redir_output_basic(void) {
    char a0[] = "echo"; char a1[] = "hello"; char a2[] = ">"; char a3[] = "file";
    char *argv[] = {a0, a1, a2, a3};
    int argc = 4;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "hello");
    ASSERT_STR_EQ(r.out_file, "file");
    ASSERT_EQ(r.append, 0);
    ASSERT_TRUE(r.in_file == (void *)0);
    return 0;
}

static int test_redir_append_basic(void) {
    char a0[] = "echo"; char a1[] = "hello"; char a2[] = ">>"; char a3[] = "file";
    char *argv[] = {a0, a1, a2, a3};
    int argc = 4;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(r.out_file, "file");
    ASSERT_EQ(r.append, 1);
    return 0;
}

static int test_redir_input_basic(void) {
    char a0[] = "cat"; char a1[] = "<"; char a2[] = "file";
    char *argv[] = {a0, a1, a2};
    int argc = 3;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "cat");
    ASSERT_STR_EQ(r.in_file, "file");
    ASSERT_TRUE(r.out_file == (void *)0);
    return 0;
}

static int test_redir_combined(void) {
    char a0[] = "cat"; char a1[] = "<"; char a2[] = "in"; char a3[] = ">"; char a4[] = "out";
    char *argv[] = {a0, a1, a2, a3, a4};
    int argc = 5;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(r.in_file, "in");
    ASSERT_STR_EQ(r.out_file, "out");
    return 0;
}

static int test_redir_output_attached(void) {
    char a0[] = "echo"; char a1[] = ">file";
    char *argv[] = {a0, a1};
    int argc = 2;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(r.out_file, "file");
    ASSERT_EQ(r.append, 0);
    return 0;
}

static int test_redir_append_attached(void) {
    char a0[] = "echo"; char a1[] = ">>file";
    char *argv[] = {a0, a1};
    int argc = 2;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(r.out_file, "file");
    ASSERT_EQ(r.append, 1);
    return 0;
}

static int test_redir_input_attached(void) {
    char a0[] = "cat"; char a1[] = "<file";
    char *argv[] = {a0, a1};
    int argc = 2;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(r.in_file, "file");
    return 0;
}

static int test_redir_no_redirect(void) {
    char a0[] = "ls"; char a1[] = "/boot";
    char *argv[] = {a0, a1};
    int argc = 2;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 2);
    ASSERT_TRUE(r.in_file == (void *)0);
    ASSERT_TRUE(r.out_file == (void *)0);
    return 0;
}

static int test_redir_missing_filename(void) {
    char a0[] = "echo"; char a1[] = ">";
    char *argv[] = {a0, a1};
    int argc = 2;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), -1);
    return 0;
}

static int test_redir_at_start(void) {
    char a0[] = ">"; char a1[] = "file"; char a2[] = "echo"; char a3[] = "hello";
    char *argv[] = {a0, a1, a2, a3};
    int argc = 4;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "hello");
    ASSERT_STR_EQ(r.out_file, "file");
    return 0;
}

static int test_redir_multiple_output(void) {
    char a0[] = "echo"; char a1[] = ">"; char a2[] = "a"; char a3[] = ">"; char a4[] = "b";
    char *argv[] = {a0, a1, a2, a3, a4};
    int argc = 5;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(r.out_file, "b");
    return 0;
}

static int test_redir_input_and_append(void) {
    char a0[] = "cat"; char a1[] = "<"; char a2[] = "in"; char a3[] = ">>"; char a4[] = "log";
    char *argv[] = {a0, a1, a2, a3, a4};
    int argc = 5;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(r.in_file, "in");
    ASSERT_STR_EQ(r.out_file, "log");
    ASSERT_EQ(r.append, 1);
    return 0;
}

static int test_redir_empty_command(void) {
    char a0[] = ">"; char a1[] = "file";
    char *argv[] = {a0, a1};
    int argc = 2;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 0);
    ASSERT_STR_EQ(r.out_file, "file");
    return 0;
}

static int test_redir_preserves_args(void) {
    char a0[] = "echo"; char a1[] = "a"; char a2[] = "b"; char a3[] = ">"; char a4[] = "f";
    char *argv[] = {a0, a1, a2, a3, a4};
    int argc = 5;
    Redirect r;
    ASSERT_EQ(parse_redirects(&argc, argv, &r), 0);
    ASSERT_EQ(argc, 3);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "a");
    ASSERT_STR_EQ(argv[2], "b");
    ASSERT_STR_EQ(r.out_file, "f");
    return 0;
}

/* =========================================================================
 * NEW TESTS — String helpers
 * ========================================================================= */

static int test_strlcpy_basic(void) {
    char dst[16];
    uint64_t r = sh_strlcpy(dst, "hello", 16);
    ASSERT_EQ(r, 5);
    ASSERT_STR_EQ(dst, "hello");
    return 0;
}

static int test_strlcpy_truncate(void) {
    char dst[4];
    uint64_t r = sh_strlcpy(dst, "hello", 4);
    ASSERT_EQ(r, 5);  /* returns source length */
    ASSERT_STR_EQ(dst, "hel");
    return 0;
}

static int test_strchr_found(void) {
    char *p = sh_strchr("hello", 'l');
    ASSERT_TRUE(p != (void *)0);
    ASSERT_EQ(*p, 'l');
    ASSERT_EQ((int)(p - "hello"), 2);
    return 0;
}

static int test_strchr_not_found(void) {
    char *p = sh_strchr("hello", 'z');
    ASSERT_TRUE(p == (void *)0);
    return 0;
}

static int test_strncmp_equal(void) {
    ASSERT_EQ(sh_strncmp("abcdef", "abcxyz", 3), 0);
    return 0;
}

static int test_strncmp_differ(void) {
    ASSERT_TRUE(sh_strncmp("abc", "abd", 3) < 0);
    return 0;
}

/* =========================================================================
 * NEW TESTS — Variables
 * ========================================================================= */

static int test_var_set_get(void) {
    vars_reset();
    ASSERT_EQ(shell_setvar("FOO", "bar"), 0);
    const char *v = shell_getvar("FOO");
    ASSERT_TRUE(v != (void *)0);
    ASSERT_STR_EQ(v, "bar");
    vars_reset();
    return 0;
}

static int test_var_overwrite(void) {
    vars_reset();
    shell_setvar("X", "old");
    shell_setvar("X", "new");
    ASSERT_STR_EQ(shell_getvar("X"), "new");
    ASSERT_EQ(var_count, 1);  /* no duplicate */
    vars_reset();
    return 0;
}

static int test_var_unset(void) {
    vars_reset();
    shell_setvar("A", "1");
    shell_setvar("B", "2");
    ASSERT_EQ(shell_unsetvar("A"), 0);
    ASSERT_TRUE(shell_getvar("A") == (void *)0);
    ASSERT_STR_EQ(shell_getvar("B"), "2");
    ASSERT_EQ(var_count, 1);
    vars_reset();
    return 0;
}

static int test_var_not_found(void) {
    vars_reset();
    ASSERT_TRUE(shell_getvar("NOPE") == (void *)0);
    ASSERT_EQ(shell_unsetvar("NOPE"), -1);
    vars_reset();
    return 0;
}

static int test_var_full(void) {
    vars_reset();
    char name[4];
    for (int i = 0; i < MAX_VARS; i++) {
        name[0] = 'A' + (char)(i / 26);
        name[1] = 'a' + (char)(i % 26);
        name[2] = '\0';
        ASSERT_EQ(shell_setvar(name, "x"), 0);
    }
    ASSERT_EQ(shell_setvar("ZZ", "overflow"), -1);
    vars_reset();
    return 0;
}

static int test_var_name_too_long(void) {
    vars_reset();
    char long_name[64];
    for (int i = 0; i < 63; i++) long_name[i] = 'A';
    long_name[63] = '\0';
    ASSERT_EQ(shell_setvar(long_name, "val"), -1);
    vars_reset();
    return 0;
}

/* =========================================================================
 * NEW TESTS — Expansion
 * ========================================================================= */

static int test_expand_no_vars(void) {
    vars_reset();
    char out[64];
    ASSERT_EQ(expand_variables("hello world", out, 64), 0);
    ASSERT_STR_EQ(out, "hello world");
    vars_reset();
    return 0;
}

static int test_expand_simple(void) {
    vars_reset();
    shell_setvar("NAME", "arc");
    char out[64];
    ASSERT_EQ(expand_variables("hello $NAME", out, 64), 0);
    ASSERT_STR_EQ(out, "hello arc");
    vars_reset();
    return 0;
}

static int test_expand_undefined(void) {
    vars_reset();
    char out[64];
    ASSERT_EQ(expand_variables("$UNDEF", out, 64), 0);
    ASSERT_STR_EQ(out, "");  /* undefined → empty */
    vars_reset();
    return 0;
}

static int test_expand_dollar_dollar(void) {
    vars_reset();
    char out[64];
    ASSERT_EQ(expand_variables("$$", out, 64), 0);
    ASSERT_STR_EQ(out, "$");
    vars_reset();
    return 0;
}

static int test_expand_single_quote(void) {
    vars_reset();
    shell_setvar("X", "val");
    char out[64];
    ASSERT_EQ(expand_variables("'$X'", out, 64), 0);
    /* Quotes passed through, $X not expanded */
    ASSERT_STR_EQ(out, "'$X'");
    vars_reset();
    return 0;
}

static int test_expand_double_quote(void) {
    vars_reset();
    shell_setvar("X", "val");
    char out[64];
    ASSERT_EQ(expand_variables("\"$X\"", out, 64), 0);
    /* Quotes passed through, $X expanded */
    ASSERT_STR_EQ(out, "\"val\"");
    vars_reset();
    return 0;
}

static int test_expand_backslash(void) {
    vars_reset();
    shell_setvar("NAME", "val");
    char out[64];
    ASSERT_EQ(expand_variables("\\$NAME", out, 64), 0);
    /* \$ → literal $, NAME is just text */
    ASSERT_STR_EQ(out, "$NAME");
    vars_reset();
    return 0;
}

static int test_expand_adjacent(void) {
    vars_reset();
    shell_setvar("A", "hello");
    shell_setvar("B", "world");
    char out[64];
    ASSERT_EQ(expand_variables("$A$B", out, 64), 0);
    ASSERT_STR_EQ(out, "helloworld");
    vars_reset();
    return 0;
}

static int test_expand_in_word(void) {
    vars_reset();
    shell_setvar("X", "mid");
    char out[64];
    ASSERT_EQ(expand_variables("pre$X.post", out, 64), 0);
    ASSERT_STR_EQ(out, "premid.post");
    vars_reset();
    return 0;
}

static int test_expand_overflow(void) {
    vars_reset();
    char val[VAR_VALUE_MAX];
    for (int i = 0; i < VAR_VALUE_MAX - 1; i++) val[i] = 'X';
    val[VAR_VALUE_MAX - 1] = '\0';
    shell_setvar("BIG", val);
    char out[16];
    ASSERT_EQ(expand_variables("$BIG", out, 16), -1);
    vars_reset();
    return 0;
}

/* =========================================================================
 * NEW TESTS — Quoting in parse_line
 * ========================================================================= */

static int test_parse_double_quotes(void) {
    char line[] = "echo \"hello world\"";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "hello world");
    return 0;
}

static int test_parse_single_quotes(void) {
    char line[] = "echo 'hello world'";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "hello world");
    return 0;
}

static int test_parse_empty_quotes(void) {
    char line[] = "echo \"\"";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[0], "echo");
    ASSERT_STR_EQ(argv[1], "");
    return 0;
}

static int test_parse_quotes_mid_word(void) {
    char line[] = "he\"ll\"o";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "hello");
    return 0;
}

static int test_parse_backslash_space(void) {
    char line[] = "hello\\ world";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "hello world");
    return 0;
}

static int test_parse_backslash_quote(void) {
    char line[] = "echo \\\"hello";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[1], "\"hello");
    return 0;
}

static int test_parse_single_in_double(void) {
    char line[] = "echo \"it's\"";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 2);
    ASSERT_STR_EQ(argv[1], "it's");
    return 0;
}

static int test_parse_mixed_quotes(void) {
    char line[] = "'say'\"hello\"";
    char *argv[MAX_ARGV];
    int argc = parse_line(line, argv);
    ASSERT_EQ(argc, 1);
    ASSERT_STR_EQ(argv[0], "sayhello");
    return 0;
}

/* =========================================================================
 * NEW TESTS — Quote-aware find_pipe
 * ========================================================================= */

static int test_pipe_basic(void) {
    char line[] = "echo hi | cat";
    char *p = find_pipe(line);
    ASSERT_TRUE(p != (void *)0);
    ASSERT_EQ(*p, '|');
    return 0;
}

static int test_pipe_in_double_quotes(void) {
    char line[] = "echo \"a|b\"";
    char *p = find_pipe(line);
    ASSERT_TRUE(p == (void *)0);
    return 0;
}

static int test_pipe_in_single_quotes(void) {
    char line[] = "echo 'a|b'";
    char *p = find_pipe(line);
    ASSERT_TRUE(p == (void *)0);
    return 0;
}

static int test_pipe_escaped(void) {
    char line[] = "echo a\\|b";
    char *p = find_pipe(line);
    ASSERT_TRUE(p == (void *)0);
    return 0;
}

static int test_pipe_after_quotes(void) {
    char line[] = "echo \"hello\" | cat";
    char *p = find_pipe(line);
    ASSERT_TRUE(p != (void *)0);
    ASSERT_EQ(*p, '|');
    return 0;
}

/* =========================================================================
 * NEW TESTS — Assignment detection
 * ========================================================================= */

static int test_assign_basic(void) {
    ASSERT_EQ(try_assignment("FOO=bar"), 1);
    return 0;
}

static int test_assign_no_equals(void) {
    ASSERT_EQ(try_assignment("FOO"), 0);
    return 0;
}

static int test_assign_empty_value(void) {
    ASSERT_EQ(try_assignment("FOO="), 1);
    return 0;
}

static int test_assign_starts_eq(void) {
    ASSERT_EQ(try_assignment("=bar"), 0);
    return 0;
}

static int test_assign_invalid_name(void) {
    ASSERT_EQ(try_assignment("1FOO=bar"), 0);
    return 0;
}

/* =========================================================================
 * Test suite export
 * ========================================================================= */

TestCase shell_tests[] = {
    /* Existing parse tests */
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
    /* Existing string tests */
    { "strcmp_equal",          test_strcmp_equal },
    { "strcmp_less",           test_strcmp_less },
    { "strcmp_greater",        test_strcmp_greater },
    { "atoi_positive",        test_atoi_positive },
    { "atoi_zero",            test_atoi_zero },
    { "atoi_negative",        test_atoi_negative },
    { "atoi_non_numeric",     test_atoi_non_numeric },
    { "strlen_basic",         test_strlen_basic },
    { "strlen_empty",         test_strlen_empty },
    /* Existing redirect tests */
    { "redir_output_basic",    test_redir_output_basic },
    { "redir_append_basic",    test_redir_append_basic },
    { "redir_input_basic",     test_redir_input_basic },
    { "redir_combined",        test_redir_combined },
    { "redir_output_attached", test_redir_output_attached },
    { "redir_append_attached", test_redir_append_attached },
    { "redir_input_attached",  test_redir_input_attached },
    { "redir_no_redirect",     test_redir_no_redirect },
    { "redir_missing_filename",test_redir_missing_filename },
    { "redir_at_start",        test_redir_at_start },
    { "redir_multiple_output", test_redir_multiple_output },
    { "redir_input_and_append",test_redir_input_and_append },
    { "redir_empty_command",   test_redir_empty_command },
    { "redir_preserves_args",  test_redir_preserves_args },
    /* NEW: String helpers */
    { "strlcpy_basic",        test_strlcpy_basic },
    { "strlcpy_truncate",     test_strlcpy_truncate },
    { "strchr_found",         test_strchr_found },
    { "strchr_not_found",     test_strchr_not_found },
    { "strncmp_equal",        test_strncmp_equal },
    { "strncmp_differ",       test_strncmp_differ },
    /* NEW: Variables */
    { "var_set_get",           test_var_set_get },
    { "var_overwrite",         test_var_overwrite },
    { "var_unset",             test_var_unset },
    { "var_not_found",         test_var_not_found },
    { "var_full",              test_var_full },
    { "var_name_too_long",     test_var_name_too_long },
    /* NEW: Expansion */
    { "expand_no_vars",        test_expand_no_vars },
    { "expand_simple",         test_expand_simple },
    { "expand_undefined",      test_expand_undefined },
    { "expand_dollar_dollar",  test_expand_dollar_dollar },
    { "expand_single_quote",   test_expand_single_quote },
    { "expand_double_quote",   test_expand_double_quote },
    { "expand_backslash",      test_expand_backslash },
    { "expand_adjacent",       test_expand_adjacent },
    { "expand_in_word",        test_expand_in_word },
    { "expand_overflow",       test_expand_overflow },
    /* NEW: Quoting in parse_line */
    { "parse_double_quotes",   test_parse_double_quotes },
    { "parse_single_quotes",   test_parse_single_quotes },
    { "parse_empty_quotes",    test_parse_empty_quotes },
    { "parse_quotes_mid_word", test_parse_quotes_mid_word },
    { "parse_backslash_space", test_parse_backslash_space },
    { "parse_backslash_quote", test_parse_backslash_quote },
    { "parse_single_in_double",test_parse_single_in_double },
    { "parse_mixed_quotes",    test_parse_mixed_quotes },
    /* NEW: Quote-aware find_pipe */
    { "pipe_basic",            test_pipe_basic },
    { "pipe_in_double_quotes", test_pipe_in_double_quotes },
    { "pipe_in_single_quotes", test_pipe_in_single_quotes },
    { "pipe_escaped",          test_pipe_escaped },
    { "pipe_after_quotes",     test_pipe_after_quotes },
    /* NEW: Assignment detection */
    { "assign_basic",          test_assign_basic },
    { "assign_no_equals",      test_assign_no_equals },
    { "assign_empty_value",    test_assign_empty_value },
    { "assign_starts_eq",      test_assign_starts_eq },
    { "assign_invalid_name",   test_assign_invalid_name },
};

int shell_test_count = sizeof(shell_tests) / sizeof(shell_tests[0]);
