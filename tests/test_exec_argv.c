/* arc_os — Host-side tests for the System V initial stack (kernel/proc/exec_stack.c).
 *
 * The suite keeps its old name (exec_argv): it tested execve's argv copy, which
 * M0-06 replaced with exec_stack.c (argv, envp and auxv; ADR-0002). */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Kernel lib headers: the host's memcpy/strlen stand in */
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_STRING_H

#include "../kernel/proc/exec_stack.c"

#define TOP 0x00007FFFFFFFE000ULL   /* USER_STACK_TOP */

static uint8_t stack_buf[EXEC_STACK_BUF];
static ExecStrings strings;

/* Host pointer for a user address inside stack_buf */
static void *at(uint64_t uaddr) {
    return stack_buf + (uaddr - (TOP - EXEC_STACK_BUF));
}

static uint64_t word(uint64_t uaddr) {
    uint64_t v;
    memcpy(&v, at(uaddr), 8);
    return v;
}

static ExecAux sample_aux(void) {
    ExecAux a;
    memset(&a, 0, sizeof(a));
    a.phdr = 0x400040;
    a.phent = 56;
    a.phnum = 5;
    a.entry = 0x401000;
    a.uid = 1000; a.euid = 1001; a.gid = 100; a.egid = 101;
    a.hwcap = 0x178BFBFF;
    for (int i = 0; i < 16; i++) a.random[i] = (uint8_t)(0xA0 + i);
    return a;
}

/* Value of auxv key in the vector starting at uaddr, or -1 if absent */
static int64_t aux_get(uint64_t auxv, uint64_t key) {
    for (uint64_t p = auxv; ; p += 16) {
        uint64_t k = word(p);
        if (k == key) return (int64_t)word(p + 8);
        if (k == AT_NULL) return -1;
    }
}

static uint64_t build(int argc, int envc, const char *execfn) {
    char s[32];
    exec_strings_init(&strings);
    for (int i = 0; i < argc; i++) {
        snprintf(s, sizeof(s), "arg%d", i);
        if (exec_strings_add(&strings, s, 0) != 0) return 0;
    }
    for (int i = 0; i < envc; i++) {
        snprintf(s, sizeof(s), "VAR%d=v%d", i, i);
        if (exec_strings_add(&strings, s, 1) != 0) return 0;
    }
    ExecAux aux = sample_aux();
    memset(stack_buf, 0xEE, sizeof(stack_buf));
    return exec_build_stack(stack_buf, sizeof(stack_buf), TOP, &strings, execfn, &aux);
}

TEST(layout_argv_envp_auxv) {
    uint64_t rsp = build(2, 3, "/boot/prog");
    ASSERT_TRUE(rsp != 0);
    ASSERT_EQ(rsp % 16, 0);
    ASSERT_EQ(word(TOP - 8), 0);                 /* top word stays zero */

    ASSERT_EQ(word(rsp), 2);                     /* argc */
    ASSERT_STR_EQ((char *)at(word(rsp + 8)), "arg0");
    ASSERT_STR_EQ((char *)at(word(rsp + 16)), "arg1");
    ASSERT_EQ(word(rsp + 24), 0);                /* argv NULL */
    ASSERT_STR_EQ((char *)at(word(rsp + 32)), "VAR0=v0");
    ASSERT_STR_EQ((char *)at(word(rsp + 48)), "VAR2=v2");
    ASSERT_EQ(word(rsp + 56), 0);                /* envp NULL */

    uint64_t auxv = rsp + 64;
    ASSERT_EQ(aux_get(auxv, AT_PHDR), 0x400040);
    ASSERT_EQ(aux_get(auxv, AT_PHENT), 56);
    ASSERT_EQ(aux_get(auxv, AT_PHNUM), 5);
    ASSERT_EQ(aux_get(auxv, AT_PAGESZ), 4096);
    ASSERT_EQ(aux_get(auxv, AT_BASE), 0);
    ASSERT_EQ(aux_get(auxv, AT_FLAGS), 0);
    ASSERT_EQ(aux_get(auxv, AT_ENTRY), 0x401000);
    ASSERT_EQ(aux_get(auxv, AT_UID), 1000);
    ASSERT_EQ(aux_get(auxv, AT_EUID), 1001);
    ASSERT_EQ(aux_get(auxv, AT_GID), 100);
    ASSERT_EQ(aux_get(auxv, AT_EGID), 101);
    ASSERT_EQ(aux_get(auxv, AT_SECURE), 0);
    ASSERT_EQ(aux_get(auxv, AT_HWCAP), 0x178BFBFF);
    ASSERT_EQ(aux_get(auxv, AT_HWCAP2), 0);

    int64_t rnd = aux_get(auxv, AT_RANDOM);
    ASSERT_TRUE(rnd > 0);
    ASSERT_EQ(rnd % 16, 0);
    ExecAux expect = sample_aux();
    ASSERT_MEM_EQ(at((uint64_t)rnd), expect.random, 16);
    ASSERT_STR_EQ((char *)at((uint64_t)aux_get(auxv, AT_EXECFN)), "/boot/prog");
    ASSERT_STR_EQ((char *)at((uint64_t)aux_get(auxv, AT_PLATFORM)), "x86_64");
    ASSERT_EQ(aux_get(auxv, 9999), -1);          /* AT_NULL ends the vector */
    return 0;
}

TEST(everything_between_rsp_and_top) {
    uint64_t rsp = build(3, 2, "/x");
    ASSERT_TRUE(rsp != 0);
    for (uint64_t i = 0; i < 3 + 2; i++) {
        uint64_t slot = (i < 3) ? rsp + 8 + 8 * i : rsp + 8 + 8 * (3 + 1) + 8 * (i - 3);
        uint64_t p = word(slot);
        ASSERT_TRUE(p > rsp && p < TOP);
    }
    /* The pointer block ends below AT_RANDOM's bytes (no overlap) */
    uint64_t auxv = rsp + 8 * (1 + 4 + 3);
    uint64_t end = auxv;
    while (word(end) != AT_NULL) end += 16;
    ASSERT_TRUE(end + 16 <= (uint64_t)aux_get(auxv, AT_RANDOM));
    return 0;
}

TEST(rsp_aligned_for_every_count_parity) {
    for (int argc = 0; argc < 4; argc++) {
        for (int envc = 0; envc < 4; envc++) {
            uint64_t rsp = build(argc, envc, "/p");
            ASSERT_TRUE(rsp != 0);
            ASSERT_EQ(rsp % 16, 0);
            ASSERT_EQ(word(rsp), (uint64_t)argc);
            ASSERT_EQ(word(rsp + 8 * (1 + argc)), 0);            /* argv NULL */
            ASSERT_EQ(word(rsp + 8 * (2 + argc + envc)), 0);     /* envp NULL */
        }
    }
    return 0;
}

TEST(strings_count_limit) {
    exec_strings_init(&strings);
    for (int i = 0; i < EXEC_MAX_PTRS; i++) {
        ASSERT_EQ(exec_strings_add(&strings, "", 0), 0);
    }
    ASSERT_EQ(exec_strings_add(&strings, "", 0), -E2BIG);
    ASSERT_EQ(exec_strings_add(&strings, "X=1", 1), -E2BIG);
    ASSERT_EQ(strings.argc, EXEC_MAX_PTRS);
    return 0;
}

TEST(strings_byte_limit_counts_pointers) {
    static char big[1024];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    exec_strings_init(&strings);
    int added = 0;
    while (exec_strings_add(&strings, big, 0) == 0) added++;
    /* Each entry costs 1024 string bytes + an 8-byte pointer */
    ASSERT_EQ(added, (EXEC_ARG_MAX - 16) / (1024 + 8));
    ASSERT_TRUE(exec_strings_cost(&strings) <= EXEC_ARG_MAX);
    /* A full set still builds: EXEC_STACK_BUF covers the worst case */
    ExecAux aux = sample_aux();
    ASSERT_TRUE(exec_build_stack(stack_buf, sizeof(stack_buf), TOP, &strings, "/p", &aux) != 0);
    return 0;
}

TEST(worst_case_fits_buffer) {
    /* Many tiny strings maximise the pointer count */
    exec_strings_init(&strings);
    while (exec_strings_add(&strings, "", strings.argc >= 500) == 0) { }
    static char path[512];
    memset(path, 'p', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    ExecAux aux = sample_aux();
    uint64_t rsp = exec_build_stack(stack_buf, sizeof(stack_buf), TOP, &strings, path, &aux);
    ASSERT_TRUE(rsp != 0);
    ASSERT_TRUE(rsp >= TOP - EXEC_STACK_BUF);
    return 0;
}

TEST(small_buffer_refused) {
    exec_strings_init(&strings);
    ASSERT_EQ(exec_strings_add(&strings, "a", 0), 0);
    ExecAux aux = sample_aux();
    ASSERT_EQ(exec_build_stack(stack_buf, 128, TOP, &strings, "/p", &aux), 0);
    return 0;
}

TEST(envp_strings_follow_argv) {
    exec_strings_init(&strings);
    ASSERT_EQ(exec_strings_add(&strings, "prog", 0), 0);
    ASSERT_EQ(exec_strings_add(&strings, "HOME=/", 1), 0);
    ASSERT_EQ(strings.argc, 1);
    ASSERT_EQ(strings.envc, 1);
    ASSERT_STR_EQ(strings.data + strings.off[0], "prog");
    ASSERT_STR_EQ(strings.data + strings.off[1], "HOME=/");
    ASSERT_EQ(exec_strings_cost(&strings), 5 + 7 + 8 * 4);
    return 0;
}

TestCase exec_argv_tests[] = {
    TEST_ENTRY(layout_argv_envp_auxv),
    TEST_ENTRY(everything_between_rsp_and_top),
    TEST_ENTRY(rsp_aligned_for_every_count_parity),
    TEST_ENTRY(strings_count_limit),
    TEST_ENTRY(strings_byte_limit_counts_pointers),
    TEST_ENTRY(worst_case_fits_buffer),
    TEST_ENTRY(small_buffer_refused),
    TEST_ENTRY(envp_strings_follow_argv),
};
int exec_argv_test_count = sizeof(exec_argv_tests) / sizeof(exec_argv_tests[0]);
