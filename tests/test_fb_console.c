/* arc_os — Host-side tests for framebuffer console */

#include "test_framework.h"
#include <stdint.h>
#include <stdbool.h>

/* Guard kernel headers */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_BOOT_BOOTINFO_H

static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Framebuffer types (standalone for testing) */
typedef struct {
    void    *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
} Framebuffer;

/* Include the implementations */
#include "../kernel/drivers/font8x16.c"
#include "../kernel/drivers/fb_console.c"

/* Test framebuffer: 160x128, 32bpp, BGRA format (small to avoid large BSS) */
#define TEST_FB_W  160
#define TEST_FB_H  128
#define TEST_FB_BPP 32
static uint32_t test_fb_pixels[TEST_FB_W * TEST_FB_H];

static Framebuffer make_test_fb(void) {
    memset(test_fb_pixels, 0, sizeof(test_fb_pixels));
    return (Framebuffer){
        .address = test_fb_pixels,
        .width = TEST_FB_W,
        .height = TEST_FB_H,
        .pitch = TEST_FB_W * (TEST_FB_BPP / 8),
        .bpp = TEST_FB_BPP,
        .red_mask_size = 8,
        .red_mask_shift = 16,
        .green_mask_size = 8,
        .green_mask_shift = 8,
        .blue_mask_size = 8,
        .blue_mask_shift = 0,
    };
}

/* --- Tests --- */

TEST(fb_init_success) {
    Framebuffer fb = make_test_fb();
    fb_initialized = 0;
    ASSERT_EQ(fb_console_init(&fb), 0);
    ASSERT_EQ(cols, TEST_FB_W / FONT_WIDTH);   /* 80 */
    ASSERT_EQ(rows, TEST_FB_H / FONT_HEIGHT);  /* 30 */
    return 0;
}

TEST(fb_init_null_fails) {
    ASSERT_EQ(fb_console_init(NULL), -1);
    return 0;
}

TEST(fb_init_low_bpp_fails) {
    Framebuffer fb = make_test_fb();
    fb.bpp = 16;
    ASSERT_EQ(fb_console_init(&fb), -1);
    return 0;
}

TEST(fb_putchar_advances_cursor) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    ASSERT_EQ(cur_col, 0);
    ASSERT_EQ(cur_row, 0);
    fb_console_putchar('A');
    ASSERT_EQ(cur_col, 1);
    ASSERT_EQ(cur_row, 0);
    fb_console_putchar('B');
    ASSERT_EQ(cur_col, 2);
    return 0;
}

TEST(fb_newline) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_putchar('A');
    fb_console_putchar('\n');
    ASSERT_EQ(cur_col, 0);
    ASSERT_EQ(cur_row, 1);
    return 0;
}

TEST(fb_carriage_return) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_puts("Hello");
    ASSERT_EQ(cur_col, 5);
    fb_console_putchar('\r');
    ASSERT_EQ(cur_col, 0);
    ASSERT_EQ(cur_row, 0);
    return 0;
}

TEST(fb_tab) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_putchar('\t');
    ASSERT_EQ(cur_col, 8);
    fb_console_putchar('X');
    ASSERT_EQ(cur_col, 9);
    fb_console_putchar('\t');
    ASSERT_EQ(cur_col, 16);
    return 0;
}

TEST(fb_backspace) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_puts("AB");
    ASSERT_EQ(cur_col, 2);
    fb_console_putchar('\b');
    ASSERT_EQ(cur_col, 1);
    return 0;
}

TEST(fb_backspace_at_start) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_putchar('\b');
    ASSERT_EQ(cur_col, 0);  /* Should not go negative */
    return 0;
}

TEST(fb_line_wrap) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    /* Fill first row (80 chars for 640px / 8px) */
    for (uint32_t i = 0; i < cols; i++) {
        fb_console_putchar('X');
    }
    ASSERT_EQ(cur_col, 0);
    ASSERT_EQ(cur_row, 1);
    return 0;
}

TEST(fb_scroll) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    /* Fill all rows */
    for (uint32_t r = 0; r < rows; r++) {
        fb_console_putchar('\n');
    }
    /* Should have scrolled, cursor at last row */
    ASSERT_EQ(cur_row, rows - 1);
    return 0;
}

TEST(fb_glyph_renders_pixels) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_set_colors(0xFFFFFF, 0x000000);
    fb_console_putchar('A');

    /* Check that some pixels in the 'A' glyph area are non-zero (foreground) */
    int fg_count = 0;
    for (int y = 0; y < FONT_HEIGHT; y++) {
        for (int x = 0; x < FONT_WIDTH; x++) {
            if (test_fb_pixels[y * TEST_FB_W + x] != 0) {
                fg_count++;
            }
        }
    }
    ASSERT_TRUE(fg_count > 0);  /* 'A' should have foreground pixels */
    return 0;
}

TEST(fb_clear_resets) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_puts("Hello World\n");
    fb_console_clear();
    ASSERT_EQ(cur_col, 0);
    ASSERT_EQ(cur_row, 0);
    return 0;
}

TEST(fb_puts_string) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_puts("Hi");
    ASSERT_EQ(cur_col, 2);
    ASSERT_EQ(cur_row, 0);
    return 0;
}

TEST(fb_control_chars_ignored) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    fb_console_putchar(0x01); /* Non-printable */
    fb_console_putchar(0x7F); /* DEL */
    ASSERT_EQ(cur_col, 0); /* Should not advance */
    return 0;
}

/* --- Suite --- */

TestCase fb_console_tests[] = {
    TEST_ENTRY(fb_init_success),
    TEST_ENTRY(fb_init_null_fails),
    TEST_ENTRY(fb_init_low_bpp_fails),
    TEST_ENTRY(fb_putchar_advances_cursor),
    TEST_ENTRY(fb_newline),
    TEST_ENTRY(fb_carriage_return),
    TEST_ENTRY(fb_tab),
    TEST_ENTRY(fb_backspace),
    TEST_ENTRY(fb_backspace_at_start),
    TEST_ENTRY(fb_line_wrap),
    TEST_ENTRY(fb_scroll),
    TEST_ENTRY(fb_glyph_renders_pixels),
    TEST_ENTRY(fb_clear_resets),
    TEST_ENTRY(fb_puts_string),
    TEST_ENTRY(fb_control_chars_ignored),
};
int fb_console_test_count = sizeof(fb_console_tests) / sizeof(fb_console_tests[0]);
