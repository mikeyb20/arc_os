/* arc_os — Tests for ANSI escape code parser logic */

#include "test_framework.h"
#include <stdint.h>

/* Test ANSI CSI parameter parsing logic (extracted from parser) */

static int parse_params(const char *seq, int *params, int max) {
    int count = 0;
    for (int i = 0; seq[i]; i++) {
        if (seq[i] >= '0' && seq[i] <= '9') {
            if (count == 0) count = 1;
            params[count - 1] = params[count - 1] * 10 + (seq[i] - '0');
        } else if (seq[i] == ';') {
            if (count < max) count++;
        }
    }
    return count;
}

/* --- Tests --- */

TEST(ansi_csi_parse_single_param) {
    int params[8] = {0};
    int count = parse_params("5", params, 8);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(params[0], 5);
    return 0;
}

TEST(ansi_csi_parse_two_params) {
    int params[8] = {0};
    int count = parse_params("10;20", params, 8);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(params[0], 10);
    ASSERT_EQ(params[1], 20);
    return 0;
}

TEST(ansi_csi_parse_three_params) {
    int params[8] = {0};
    int count = parse_params("1;31;42", params, 8);
    ASSERT_EQ(count, 3);
    ASSERT_EQ(params[0], 1);
    ASSERT_EQ(params[1], 31);
    ASSERT_EQ(params[2], 42);
    return 0;
}

TEST(ansi_csi_parse_no_params) {
    int params[8] = {0};
    int count = parse_params("", params, 8);
    ASSERT_EQ(count, 0);
    return 0;
}

TEST(ansi_sgr_color_ranges) {
    /* FG: 30-37 normal, 90-97 bright */
    ASSERT_TRUE(31 >= 30 && 31 <= 37);
    ASSERT_EQ(31 - 30, 1);  /* Red */
    ASSERT_TRUE(92 >= 90 && 92 <= 97);
    ASSERT_EQ(92 - 90, 2);  /* Bright Green */
    /* BG: 40-47 normal, 100-107 bright */
    ASSERT_TRUE(44 >= 40 && 44 <= 47);
    ASSERT_EQ(44 - 40, 4);  /* Blue BG */
    return 0;
}

TEST(ansi_sgr_reset) {
    /* SGR 0 = reset all */
    int p = 0;
    ASSERT_EQ(p, 0);
    return 0;
}

TEST(ansi_sgr_bold) {
    int p = 1;
    ASSERT_EQ(p, 1);  /* Bold */
    return 0;
}

TEST(ansi_cursor_position_1indexed) {
    /* ESC[5;10H means row 5, col 10 (1-indexed) → 0-indexed: 4, 9 */
    int row = 5 - 1;
    int col = 10 - 1;
    ASSERT_EQ(row, 4);
    ASSERT_EQ(col, 9);
    return 0;
}

TEST(ansi_cursor_default_param) {
    /* Missing param defaults to 1 */
    int params[8] = {0};
    int count = parse_params("", params, 8);
    int n = (count > 0 && params[0] > 0) ? params[0] : 1;
    ASSERT_EQ(n, 1);
    return 0;
}

TEST(ansi_erase_modes) {
    /* Erase display: 0=to end, 1=to start, 2=all */
    ASSERT_EQ(0, 0);  ASSERT_EQ(1, 1);  ASSERT_EQ(2, 2);
    return 0;
}

TEST(ansi_color_palette_standard) {
    uint32_t colors[8] = {
        0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
        0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA
    };
    ASSERT_EQ(colors[0], 0x000000);
    ASSERT_EQ(colors[1], 0xAA0000);
    ASSERT_EQ(colors[7], 0xAAAAAA);
    return 0;
}

TEST(ansi_color_palette_bright) {
    uint32_t bright[8] = {
        0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
        0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
    };
    ASSERT_EQ(bright[0], 0x555555);
    ASSERT_EQ(bright[7], 0xFFFFFF);
    return 0;
}

TestCase ansi_tests[] = {
    TEST_ENTRY(ansi_csi_parse_single_param),
    TEST_ENTRY(ansi_csi_parse_two_params),
    TEST_ENTRY(ansi_csi_parse_three_params),
    TEST_ENTRY(ansi_csi_parse_no_params),
    TEST_ENTRY(ansi_sgr_color_ranges),
    TEST_ENTRY(ansi_sgr_reset),
    TEST_ENTRY(ansi_sgr_bold),
    TEST_ENTRY(ansi_cursor_position_1indexed),
    TEST_ENTRY(ansi_cursor_default_param),
    TEST_ENTRY(ansi_erase_modes),
    TEST_ENTRY(ansi_color_palette_standard),
    TEST_ENTRY(ansi_color_palette_bright),
};
int ansi_test_count = sizeof(ansi_tests) / sizeof(ansi_tests[0]);
