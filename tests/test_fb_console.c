/* arc_os — Host-side tests for framebuffer console */

#include "test_framework.h"
#include <stdint.h>
#include <stdbool.h>

/* Guard kernel headers. The log ring (lib/klog.c) and ksnprintf are the real
 * ones, built by test_kprintf.c. */
#define ARCHOS_LIB_MEM_H
#define ARCHOS_BOOT_BOOTINFO_H

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

/* Before the cell array is attached (early boot) a full screen clears and
 * output restarts at the top: the framebuffer is never read back */
TEST(fb_scroll_without_cells_wraps) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    for (uint32_t r = 0; r < rows; r++) {
        fb_console_putchar('\n');
    }
    ASSERT_EQ(cur_row, 0);
    return 0;
}

/* Count foreground pixels in text cell (col, row) */
static int cell_fg_pixels(uint32_t col, uint32_t row, uint32_t fg) {
    int n = 0;
    for (uint32_t y = 0; y < FONT_HEIGHT * scale; y++) {
        for (uint32_t x = 0; x < FONT_WIDTH * scale; x++) {
            uint64_t px = (uint64_t)col * FONT_WIDTH * scale + x, py = (uint64_t)row * FONT_HEIGHT * scale + y;
            if (test_fb_pixels[py * TEST_FB_W + px] == fg) n++;
        }
    }
    return n;
}

static FbCell test_cells[(TEST_FB_W / FONT_WIDTH) * (TEST_FB_H / FONT_HEIGHT)];

static void init_with_cells(Framebuffer *fb) {
    fb_console_init(fb);
    if (fb_console_cells_bytes() != sizeof(test_cells)) abort();   /* test setup is wrong */
    fb_console_attach_cells(test_cells);
}

TEST(fb_scroll_with_cells_moves_text) {
    Framebuffer fb = make_test_fb();
    init_with_cells(&fb);
    fb_console_set_colors(0xFFFFFF, 0x000000);
    uint32_t white = pack_color(0xFFFFFF);
    fb_console_putchar('\n');
    fb_console_putchar('A');            /* row 1 */
    for (uint32_t r = 1; r < rows; r++) fb_console_putchar('\n');   /* one scroll */
    ASSERT_EQ(cur_row, rows - 1);
    ASSERT_EQ(test_cells[0].ch, 'A');    /* moved up to row 0 */
    ASSERT_EQ(test_cells[cols].ch, ' ');
    ASSERT_TRUE(cell_fg_pixels(0, 0, white) > 0);
    ASSERT_EQ(cell_fg_pixels(0, 1, white), 0);
    return 0;
}

/* Scrolling redraws only cells that change: pixels of an unchanged cell are
 * left as they are (proof that nothing is copied or read back) */
TEST(fb_scroll_redraws_only_changes) {
    Framebuffer fb = make_test_fb();
    init_with_cells(&fb);
    /* Rows 0 and 1 both blank: after a scroll row 0 is unchanged */
    test_fb_pixels[0] = 0x12345678;
    fb_console_set_cursor(0, rows - 1);
    fb_console_putchar('\n');
    ASSERT_EQ(test_fb_pixels[0], 0x12345678u);
    return 0;
}

TEST(fb_scale_2x_on_large_screens) {
    Framebuffer fb = make_test_fb();
    static uint8_t big[2400 * 1350 * 4];
    fb.address = big;
    fb.width = 2400;
    fb.height = 1350;
    fb.pitch = 2400 * 4;
    ASSERT_EQ(fb_console_init(&fb), 0);
    ASSERT_EQ(scale, 2);
    ASSERT_EQ(fb_console_cols(), 150);   /* 2400 / 16 */
    ASSERT_EQ(fb_console_rows(), 42);    /* 1350 / 32 */
    fb.width = 2399;
    fb.pitch = 2399 * 4;
    fb_console_init(&fb);
    ASSERT_EQ(scale, 1);
    return 0;
}

/* 24 bpp with a BGR channel order: colours follow the framebuffer's masks */
TEST(fb_24bpp_bgr_masks) {
    Framebuffer fb = make_test_fb();
    fb.bpp = 24;
    fb.pitch = TEST_FB_W * 3;
    fb.red_mask_shift = 0;
    fb.blue_mask_shift = 16;
    ASSERT_EQ(fb_console_init(&fb), 0);
    fb_console_write_row(0, "#", 0xFF0000, 0x0000FF);   /* red on blue */
    uint8_t *p = (uint8_t *)test_fb_pixels;
    /* Top-left pixel of '#' is background: blue = shift 16 = third byte */
    ASSERT_EQ(p[0], 0x00);
    ASSERT_EQ(p[1], 0x00);
    ASSERT_EQ(p[2], 0xFF);
    /* Somewhere in the glyph a red pixel: first byte */
    int red = 0;
    for (uint32_t y = 0; y < FONT_HEIGHT; y++)
        for (uint32_t x = 0; x < FONT_WIDTH; x++) {
            uint8_t *q = p + y * fb.pitch + x * 3;
            if (q[0] == 0xFF && q[1] == 0 && q[2] == 0) red++;
        }
    ASSERT_TRUE(red > 0);
    /* The pixel after the last column of a 24 bpp row is untouched */
    ASSERT_EQ(p[TEST_FB_W * 3], 0);
    return 0;
}

TEST(fb_write_row_pads_and_replaces) {
    Framebuffer fb = make_test_fb();
    init_with_cells(&fb);
    fb_console_write_row(2, "ab\x01", 0xFFFFFF, 0x000000);
    ASSERT_EQ(test_cells[2 * cols + 0].ch, 'a');
    ASSERT_EQ(test_cells[2 * cols + 2].ch, '?');   /* control character */
    ASSERT_EQ(test_cells[2 * cols + cols - 1].ch, ' ');
    ASSERT_EQ(cur_row, 0);                           /* cursor untouched */
    fb_console_write_row(rows, "x", 0, 0);           /* out of range: ignored */
    return 0;
}

/* Collects the log lines log_lines() returns */
static char got_lines[64][KLOG_LINE_MAX + 1];
static uint32_t got_count;
static void collect_line(uint32_t index, const char *text, uint32_t len, void *ctx) {
    (void)index;
    (void)ctx;
    if (got_count < 64) {
        memcpy(got_lines[got_count], text, len);
        got_lines[got_count][len] = '\0';
        got_count++;
    }
}

TEST(fb_log_lines_filters_levels) {
    const char a[] = "\nfb-test info\n", b[] = "fb-test debug\n", c[] = "fb-test err\n";
    klog_write(6, a, sizeof(a) - 1);
    klog_write(7, b, sizeof(b) - 1);
    klog_write(3, c, sizeof(c) - 1);
    uint32_t total = log_lines(0, 0, NULL, NULL);
    ASSERT_TRUE(total >= 2);
    got_count = 0;
    log_lines(total - 2, 2, collect_line, NULL);
    ASSERT_EQ(got_count, 2);
    ASSERT_STR_EQ(got_lines[0], "fb-test info");   /* the debug line is above the console level */
    ASSERT_STR_EQ(got_lines[1], "fb-test err");
    return 0;
}

TEST(fb_scrollback_freezes_and_returns) {
    Framebuffer fb = make_test_fb();
    init_with_cells(&fb);
    char line[32];
    for (int i = 0; i < 40; i++) {
        int n = ksnprintf(line, sizeof(line), "scroll line %d\n", i);
        klog_write(6, line, (size_t)n);
    }
    fb_console_set_colors(0xFFFFFF, 0x000000);
    uint32_t white = pack_color(0xFFFFFF);
    fb_console_scrollback(1);
    ASSERT_EQ(view_pages, 1);
    /* Output while the view is open updates the cells, not the screen */
    fb_console_set_cursor(0, rows - 1);
    int before = cell_fg_pixels(0, rows - 1, white);
    fb_console_putchar('W');
    ASSERT_EQ(test_cells[(rows - 1) * cols].ch, 'W');
    ASSERT_EQ(cell_fg_pixels(0, rows - 1, white), before);
    fb_console_scrollback(-5);   /* clamps at the live screen and redraws it */
    ASSERT_EQ(view_pages, 0);
    ASSERT_TRUE(cell_fg_pixels(0, rows - 1, white) > 0);
    fb_console_scrollback(1000);   /* clamps at the oldest page */
    ASSERT_TRUE(view_pages >= 1);
    ASSERT_TRUE(view_pages <= log_lines(0, 0, NULL, NULL) / (rows - 1));
    fb_console_scrollback(-1000);
    ASSERT_EQ(view_pages, 0);
    return 0;
}

TEST(fb_panic_view_colours_and_freezes) {
    Framebuffer fb = make_test_fb();
    init_with_cells(&fb);
    /* The newest log line, 30 columns without a space: wrapped at the 20-column width */
    klog_write(0, "\nABCDEFGHIJKLMNOPQRSTUVWXYZ0123\n", 32);
    fb_console_panic_view(10 * FONT_WIDTH);   /* under FB_PANIC_MIN_COLS: full width */
    uint32_t white = pack_color(0xFFFFFF);
    ASSERT_TRUE(cell_fg_pixels(cols - 1, rows - 2, white) > 0);   /* 'T' ends the first row */
    ASSERT_TRUE(cell_fg_pixels(9, rows - 1, white) > 0);          /* '3' ends the second */
    ASSERT_EQ(cell_fg_pixels(10, rows - 1, white), 0);
    ASSERT_EQ(test_fb_pixels[0], pack_color(0xFFFF00));                    /* banner */
    ASSERT_EQ(test_fb_pixels[FONT_HEIGHT * TEST_FB_W], pack_color(0x8B0000));   /* row 1 background */
    uint32_t snapshot = test_fb_pixels[(rows - 1) * FONT_HEIGHT * TEST_FB_W + 3];
    fb_console_puts("\n\n\nafter the panic\n");
    ASSERT_EQ(test_fb_pixels[(rows - 1) * FONT_HEIGHT * TEST_FB_W + 3], snapshot);
    view_pages = 0;
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


/* --- Pause and bitmaps (QR screens, M1-04c) --- */

/* Output while paused changes the cells but not one pixel; resume redraws */
TEST(fb_pause_keeps_pixels_resume_redraws) {
    Framebuffer fb = make_test_fb();
    init_with_cells(&fb);
    fb_console_set_colors(0xFFFFFF, 0x000000);
    uint32_t white = pack_color(0xFFFFFF);
    fb_console_pause();
    fb_console_fill(0x00FF00);   /* the pauser's own drawing */
    static uint32_t snapshot[TEST_FB_W * TEST_FB_H];
    memcpy(snapshot, test_fb_pixels, sizeof(snapshot));
    fb_console_puts("paused\n");
    for (uint32_t r = 0; r < rows + 2; r++) fb_console_putchar('\n');   /* scrolls too */
    fb_console_clear();
    fb_console_puts("X");
    fb_console_scrollback(1);   /* ignored while paused */
    ASSERT_EQ(view_pages, 0);
    ASSERT_MEM_EQ(test_fb_pixels, snapshot, sizeof(snapshot));
    ASSERT_EQ(test_cells[0].ch, 'X');
    fb_console_resume();
    ASSERT_TRUE(cell_fg_pixels(0, 0, white) > 0);       /* the 'X' is drawn now */
    ASSERT_EQ(test_fb_pixels[TEST_FB_W * (TEST_FB_H - 1)], pack_color(0x000000));   /* no green left */
    fb_console_resume();   /* a second resume does nothing */
    return 0;
}

/* Pixel (x, y) of a 32 bpp test screen is the dark (black) colour */
static int dark_at(uint32_t x, uint32_t y) {
    return test_fb_pixels[y * TEST_FB_W + x] == pack_color(0x000000);
}

/* A 3x2 bitmap at scale 2 with a 1-module border: each module is a 2x2
 * block, the border white, nothing outside the box touched */
TEST(fb_draw_bitmap_scale_and_quiet_zone) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    for (size_t i = 0; i < TEST_FB_W * TEST_FB_H; i++) test_fb_pixels[i] = 0x12345678;
    const uint8_t bits[2] = {0xA0, 0x40};   /* row 0: 1 0 1; row 1: 0 1 0 */
    fb_console_draw_bitmap(bits, 3, 2, 1, 1, 2, 10, 20);
    /* box: (3 + 2) x (2 + 2) modules = 10 x 8 pixels from (10, 20) */
    for (uint32_t y = 20; y < 28; y++) {
        for (uint32_t x = 10; x < 20; x++) {
            int mx = (int)(x - 10) / 2 - 1, my = (int)(y - 20) / 2 - 1;
            int want = mx >= 0 && my >= 0 && mx < 3 && my < 2 && ((bits[my] >> (7 - mx)) & 1);
            ASSERT_EQ(dark_at(x, y), want);
            if (!want) ASSERT_EQ(test_fb_pixels[y * TEST_FB_W + x], pack_color(0xFFFFFF));
        }
    }
    ASSERT_EQ(test_fb_pixels[20 * TEST_FB_W + 9], 0x12345678u);
    ASSERT_EQ(test_fb_pixels[20 * TEST_FB_W + 20], 0x12345678u);
    ASSERT_EQ(test_fb_pixels[19 * TEST_FB_W + 10], 0x12345678u);
    ASSERT_EQ(test_fb_pixels[28 * TEST_FB_W + 10], 0x12345678u);
    return 0;
}

/* Clipped at the screen edge (no write past the last row or column) */
TEST(fb_draw_bitmap_clips) {
    Framebuffer fb = make_test_fb();
    fb_console_init(&fb);
    static uint32_t guard[TEST_FB_W * TEST_FB_H + 64];
    memset(guard, 0, sizeof(guard));
    fb.address = guard;
    fb_console_init(&fb);
    const uint8_t bits[1] = {0xFF};
    fb_console_draw_bitmap(bits, 8, 1, 1, 4, 10, TEST_FB_W - 5, TEST_FB_H - 5);
    for (size_t i = TEST_FB_W * TEST_FB_H; i < sizeof(guard) / sizeof(guard[0]); i++) ASSERT_EQ(guard[i], 0);
    ASSERT_EQ(guard[(TEST_FB_H - 1) * TEST_FB_W + TEST_FB_W - 1], pack_color(0xFFFFFF));   /* quiet zone */
    fb_console_draw_bitmap(bits, 8, 1, 1, 0, 0, 0, 0);   /* scale 0: nothing */
    ASSERT_EQ(guard[0], 0);
    return 0;
}

/* 24 bpp BGR with a padded pitch: black and white pack through the masks,
 * and the padding bytes after each row stay untouched */
TEST(fb_draw_bitmap_24bpp_padded_pitch) {
    Framebuffer fb = make_test_fb();
    fb.bpp = 24;
    fb.width = 40;
    fb.height = 20;
    fb.pitch = 40 * 3 + 8;
    fb.red_mask_shift = 0;
    fb.blue_mask_shift = 16;
    memset(test_fb_pixels, 0xEE, sizeof(test_fb_pixels));
    ASSERT_EQ(fb_console_init(&fb), 0);
    memset(test_fb_pixels, 0xEE, sizeof(test_fb_pixels));
    const uint8_t bits[1] = {0x80};
    fb_console_draw_bitmap(bits, 1, 1, 1, 1, 4, 0, 0);   /* 12 x 12 pixels */
    uint8_t *p = (uint8_t *)test_fb_pixels;
    ASSERT_EQ(p[0], 0xFF);                               /* (0, 0): quiet zone, white */
    uint8_t *mid = p + 5 * fb.pitch + 5 * 3;              /* inside the dark module */
    ASSERT_EQ(mid[0], 0);
    ASSERT_EQ(mid[1], 0);
    ASSERT_EQ(mid[2], 0);
    for (uint32_t y = 0; y < 12; y++)
        for (uint32_t b = 40 * 3; b < fb.pitch; b++) ASSERT_EQ(p[y * fb.pitch + b], 0xEE);
    ASSERT_EQ(p[11 * fb.pitch + 12 * 3], 0xEE);           /* first pixel right of the box */
    return 0;
}

/* paint_row draws pixels only: the cells keep what the console wrote */
TEST(fb_paint_row_leaves_cells) {
    Framebuffer fb = make_test_fb();
    init_with_cells(&fb);
    fb_console_puts("keep");
    fb_console_pause();
    fb_console_paint_row(0, "drawn", 0x000000, 0xFFFFFF);
    ASSERT_EQ(test_cells[0].ch, 'k');
    ASSERT_EQ(test_fb_pixels[0], pack_color(0xFFFFFF));   /* the painted row's background */
    ASSERT_EQ(fb_console_width(), TEST_FB_W);
    ASSERT_EQ(fb_console_height(), TEST_FB_H);
    ASSERT_EQ(fb_console_row_height(), FONT_HEIGHT);
    fb_console_resume();
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
    TEST_ENTRY(fb_scroll_without_cells_wraps),
    TEST_ENTRY(fb_scroll_with_cells_moves_text),
    TEST_ENTRY(fb_scroll_redraws_only_changes),
    TEST_ENTRY(fb_scale_2x_on_large_screens),
    TEST_ENTRY(fb_24bpp_bgr_masks),
    TEST_ENTRY(fb_write_row_pads_and_replaces),
    TEST_ENTRY(fb_log_lines_filters_levels),
    TEST_ENTRY(fb_scrollback_freezes_and_returns),
    TEST_ENTRY(fb_panic_view_colours_and_freezes),
    TEST_ENTRY(fb_glyph_renders_pixels),
    TEST_ENTRY(fb_clear_resets),
    TEST_ENTRY(fb_puts_string),
    TEST_ENTRY(fb_control_chars_ignored),
    TEST_ENTRY(fb_pause_keeps_pixels_resume_redraws),
    TEST_ENTRY(fb_draw_bitmap_scale_and_quiet_zone),
    TEST_ENTRY(fb_draw_bitmap_clips),
    TEST_ENTRY(fb_draw_bitmap_24bpp_padded_pitch),
    TEST_ENTRY(fb_paint_row_leaves_cells),
};
int fb_console_test_count = sizeof(fb_console_tests) / sizeof(fb_console_tests[0]);
