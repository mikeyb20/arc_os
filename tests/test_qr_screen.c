/* arc_os — Host-side tests for kernel/diag/qr_screen.c (M1-04c)
 *
 * The codes are drawn into a host framebuffer through the real fb_console
 * (linked from test_fb_console.c), read back as a PGM image and decoded with
 * zbarimg, so the whole path from text to pixels is checked. The QR encoder
 * and text pages come from test_qr.c, ksym_lookup and its small table (foo,
 * bar, baz) from test_panic.c, the log ring from test_kprintf.c. */

#define _POSIX_C_SOURCE 200809L   /* mkstemp */

#include "test_framework.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../kernel/diag/qr_screen.c"
#include "../kernel/drivers/font8x16.h"
#include "zbar_decode.h"

/* ---- A host screen ---- */

#define SCREEN_MAX_PIXELS (1366 * 800)
static uint32_t screen[SCREEN_MAX_PIXELS];   /* 32 bpp; 24 bpp tests use it as bytes */
static Framebuffer screen_fb;

/* Start the console on a w x h screen (bpp 32 RGB, or 24 BGR with a padded pitch). */
static void screen_init(uint32_t w, uint32_t h, int bpp24) {
    memset(screen, 0x55, sizeof(screen));
    screen_fb = (Framebuffer){
        .address = screen,
        .width = w,
        .height = h,
        .pitch = bpp24 ? w * 3 + 16 : w * 4,
        .bpp = bpp24 ? 24 : 32,
        .red_mask_size = 8,
        .red_mask_shift = bpp24 ? 0 : 16,
        .green_mask_size = 8,
        .green_mask_shift = 8,
        .blue_mask_size = 8,
        .blue_mask_shift = bpp24 ? 16 : 0,
    };
    if (fb_console_init(&screen_fb) != 0) abort();
}

/* Grey level of pixel (x, y): the mean of its three channels */
static int grey_at(uint32_t x, uint32_t y) {
    const uint8_t *p = (const uint8_t *)screen + y * screen_fb.pitch + x * (screen_fb.bpp / 8);
    return (p[0] + p[1] + p[2]) / 3;
}

/* Decode the screen (or the box x0..x1, y0..y1 of it) with zbarimg. */
static long decode_screen(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t *out, size_t cap) {
    char path[] = "/tmp/arc_qrscreen_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "wb");
    if (f == NULL) return -1;
    fprintf(f, "P5\n%u %u\n255\n", x1 - x0, y1 - y0);
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = x0; x < x1; x++) fputc(grey_at(x, y), f);
    fclose(f);
    long n = zbar_decode_file(path, out, cap);
    unlink(path);
    return n;
}

/* The dark pixels' bounding box */
typedef struct {
    uint32_t x0, y0, x1, y1;   /* x1, y1 one past */
} Box;

static Box dark_box(uint32_t y_limit) {
    Box b = {UINT32_MAX, UINT32_MAX, 0, 0};
    for (uint32_t y = 0; y < y_limit; y++)
        for (uint32_t x = 0; x < screen_fb.width; x++)
            if (grey_at(x, y) < 128) {
                if (x < b.x0) b.x0 = x;
                if (y < b.y0) b.y0 = y;
                if (x + 1 > b.x1) b.x1 = x + 1;
                if (y + 1 > b.y1) b.y1 = y + 1;
            }
    return b;
}

static uint8_t decoded[4096];
static char text_buf[16384];
static char payload_buf[QR_BYTES_MAX];
static QrCode code;
static QrPanicWork work;

/* ---- Scale ---- */

TEST(qrscreen_scale) {
    /* Version 30 is 137 modules, 145 with the quiet zone (parallel-lanes §6.4) */
    ASSERT_EQ(qr_screen_scale(137, 1366, 768), 5);    /* low-tier laptop */
    ASSERT_EQ(qr_screen_scale(137, 2400, 1350), 9);   /* 2.8K panel */
    ASSERT_EQ(qr_screen_scale(137, 1280, 784), 5);    /* QEMU 1280x800, caption row off */
    ASSERT_EQ(qr_screen_scale(137, 580, 2000), 4);    /* the shorter side decides */
    ASSERT_EQ(qr_screen_scale(137, 579, 2000), 0);    /* under 4 px per module: refused */
    ASSERT_EQ(qr_screen_scale(21, 0, 0), 0);
    return 0;
}

/* ---- Report pages ---- */

static const QrTextFormat page_fmt = {.commit = "0123456789ab", .kind = "report", .max_version = 30};

/* Draw every page of text and decode each from the screen: the decoded
 * bytes equal qr_text_page's payload, and the caption names the page and its CRC. */
static int check_pages_on_screen(const char *text, size_t len) {
    QrTextPage pages[8];
    int n = qr_text_paginate(&page_fmt, text, len, pages, 8);
    ASSERT_TRUE(n >= 1);
    for (int i = 0; i < n; i++) {
        ASSERT_EQ(qr_screen_show_page(&page_fmt, text, pages, n, i, 10, &code, payload_buf), 0);
        char want[QR_BYTES_MAX];
        int wlen = qr_text_page(&page_fmt, text, pages, n, i, want, sizeof(want));
        ASSERT_TRUE(wlen > 0);
        long got = decode_screen(0, 0, screen_fb.width, screen_fb.height, decoded, sizeof(decoded));
        if (got != wlen || memcmp(decoded, want, (size_t)wlen) != 0) {
            printf("  FAIL: page %d/%d on a %lux%lu screen: decoded %ld bytes, want %d\n", i + 1, n, screen_fb.width,
                   screen_fb.height, got, wlen);
            return 1;
        }
        /* Centred above the caption row, white everywhere else */
        uint32_t rows = fb_console_rows(), avail = qr_screen_page_height();
        ASSERT_EQ(avail, (rows - 1) * fb_console_row_height());   /* the caption row's top */
        Box b = dark_box(avail);
        uint32_t scale = qr_screen_scale((uint32_t)code.size, (uint32_t)screen_fb.width, avail);
        ASSERT_EQ(b.x1 - b.x0, (uint32_t)code.size * scale);
        ASSERT_EQ(b.y1 - b.y0, (uint32_t)code.size * scale);
        uint32_t box = ((uint32_t)code.size + 2 * QR_SCREEN_QUIET) * scale;
        ASSERT_EQ(b.x0, ((uint32_t)screen_fb.width - box) / 2 + QR_SCREEN_QUIET * scale);
        ASSERT_EQ(b.y0, (avail - box) / 2 + QR_SCREEN_QUIET * scale);
        ASSERT_EQ(grey_at(0, 0), 255);
        /* The whole quiet zone under the code is white: the caption stays below it */
        uint32_t q = QR_SCREEN_QUIET * scale;
        for (uint32_t y = b.y1; y < b.y1 + q; y++)
            for (uint32_t x = b.x0 - q; x < b.x1 + q; x++)
                if (grey_at(x, y) != 255) {
                    printf("  FAIL: page %d: quiet zone pixel (%u, %u) is not white\n", i + 1, x, y);
                    return 1;
                }
        /* The caption: some dark glyph pixels in the bottom row, none below the code otherwise */
        Box cap = dark_box((uint32_t)screen_fb.height);
        ASSERT_TRUE(cap.y1 > avail);
    }
    return 0;
}

/* A report-like text of `lines` lines */
static size_t make_report(int lines) {
    size_t pos = 0;
    for (int i = 0; i < lines; i++)
        pos += (size_t)snprintf(text_buf + pos, sizeof(text_buf) - pos,
                                "  00:%02x.0 8086:29c0 class 0600 bridge       line %d of the report\n", i % 32, i);
    return pos;
}

TEST(qrscreen_report_pages_decode_32bpp) {
    screen_init(1280, 800, 0);   /* QEMU's screen */
    size_t len = make_report(60);
    ASSERT_TRUE(len > 3000);     /* several pages */
    return check_pages_on_screen(text_buf, len);
}

TEST(qrscreen_report_pages_decode_24bpp_bgr) {
    screen_init(1366, 768, 1);   /* the low-tier class, BGR, padded pitch */
    size_t len = make_report(25);
    return check_pages_on_screen(text_buf, len);
}

/* A height that is not a whole number of text rows: the caption row starts
 * above the screen's last 16 pixels, and the code stays above the caption */
TEST(qrscreen_report_pages_uneven_height) {
    screen_init(1280, 790, 0);   /* 49 rows, the caption at y = 768 */
    size_t len = make_report(20);
    return check_pages_on_screen(text_buf, len);
}

TEST(qrscreen_page_too_small_screen) {
    screen_init(400, 300, 0);   /* version 30 needs 580 x 580 at 4 px */
    size_t len = make_report(40);
    QrTextPage pages[8];
    int n = qr_text_paginate(&page_fmt, text_buf, len, pages, 8);
    ASSERT_TRUE(n >= 1);
    ASSERT_EQ(grey_at(0, 0), 0);   /* fb_console_init cleared it */
    ASSERT_EQ(qr_screen_show_page(&page_fmt, text_buf, pages, n, 0, 10, &code, payload_buf), -ENOSPC);
    ASSERT_EQ(grey_at(0, 0), 0);   /* nothing drawn */
    return 0;
}

/* ---- Panic code ---- */

static PanicInfo make_panic(const char *reason, uint32_t nframes) {
    static const char *names[] = {"RIP", "RSP",    "RBP", "RAX", "RBX", "RCX", "RDX", "RSI",
                                  "RDI", "R8",     "R9",  "R10", "R11", "R12", "R13", "R14",
                                  "R15", "RFLAGS", "CS",  "SS",  "ERR", "CR0", "CR2", "CR3"};
    PanicInfo pi = {.reason = reason, .cpu = 3, .nregs = 24, .nframes = nframes};
    for (uint32_t i = 0; i < 24; i++) {
        pi.reg_names[i] = names[i];
        pi.reg_values[i] = 0xffffffff80001000ULL + i;
    }
    for (uint32_t i = 0; i < nframes; i++) pi.frames[i] = (i % 2) ? 0x401000 + i : KSYM_BASE + 0x1010 + i;
    return pi;
}

TEST(qrscreen_panic_text_format) {
    PanicInfo pi = make_panic("Page Fault", 2);
    char out[QR_PANIC_TEXT_MAX];
    size_t len = qr_panic_text(&pi, 2, out, sizeof(out));
    ASSERT_EQ(len, strlen(out));
    ASSERT_TRUE(strncmp(out, "PANIC Page Fault\nCPU 3\nRIP=ffffffff80001000 RSP=ffffffff80001001 ", 64) == 0);
    ASSERT_TRUE(strstr(out, "CR2=ffffffff80001016 CR3=ffffffff80001017\n") != NULL);
    ASSERT_TRUE(strstr(out, "#0 ffffffff80001010 foo+0x10\n#1 0000000000401001 ?\n") != NULL);
    ASSERT_TRUE(strstr(out, "cut") == NULL);
    /* One frame kept of two */
    len = qr_panic_text(&pi, 1, out, sizeof(out));
    ASSERT_TRUE(strstr(out, "#1 ") == NULL);
    ASSERT_TRUE(strstr(out, "(1 more frames cut)\n") != NULL);
    /* A tiny buffer is cut, NUL-terminated */
    ASSERT_EQ(qr_panic_text(&pi, 2, out, 8), 7);
    ASSERT_STR_EQ(out, "PANIC P");
    return 0;
}

/* The panic code decodes from the screen and holds the whole report */
TEST(qrscreen_panic_code_decodes) {
    screen_init(1280, 800, 0);
    PanicInfo pi = make_panic("Page Fault", 16);
    int ver = qr_panic_encode(&pi, "0123456789ab-dirty", &work);
    ASSERT_TRUE(ver >= 1 && ver <= 30);
    fb_console_panic_view(0);
    ASSERT_EQ(qr_screen_draw_panic(&work.qr), 0);
    /* Top right, under the banner row, at most 3/5 of the width */
    uint32_t row_h = fb_console_row_height();
    ASSERT_EQ(qr_panic_scale(row_h), QR_SCREEN_MIN_SCALE);   /* 16-pixel rows at 1280x800 */
    ASSERT_EQ(qr_panic_scale(32), 2 * QR_SCREEN_MIN_SCALE);
    uint32_t box = ((uint32_t)work.qr.size + 2 * QR_SCREEN_QUIET) * QR_SCREEN_MIN_SCALE;
    uint32_t q = QR_SCREEN_QUIET * QR_SCREEN_MIN_SCALE;
    ASSERT_EQ(grey_at(1280 - box + q, row_h + q), 0);   /* the finder pattern's corner, 4 px modules */
    ASSERT_EQ(grey_at(1280 - box + q - 1, row_h + q), 255);
    ASSERT_EQ(grey_at(1280 - box, row_h), 255);        /* the quiet zone's corner */
    ASSERT_EQ(grey_at(1279, row_h + box - 1), 255);
    long got = decode_screen(1280 - box, row_h, 1280, row_h + box, decoded, sizeof(decoded));
    ASSERT_TRUE(got > 0);
    decoded[got] = '\0';
    ASSERT_TRUE(strncmp((char *)decoded, "ARC1 0123456789ab-dirty panic 1/1 ", 34) == 0);
    ASSERT_TRUE(strstr((char *)decoded, "\nPANIC Page Fault\nCPU 3\n") != NULL);
    ASSERT_TRUE(strstr((char *)decoded, "#15 000000000040100f ?\n") != NULL);   /* every frame fitted */
    ASSERT_TRUE(strstr((char *)decoded, "cut") == NULL);
    ASSERT_TRUE(strstr((char *)decoded, "\nall ") != NULL);
    return 0;
}

/* A reason too long for one code: frames go from the bottom and the text says so;
 * a link or a control byte in the reason comes out escaped */
TEST(qrscreen_panic_cuts_frames) {
    char reason[700];
    memset(reason, 'r', sizeof(reason));
    memcpy(reason, "see http://x \x01 ", 15);
    reason[sizeof(reason) - 1] = '\0';
    PanicInfo pi = make_panic(reason, 16);
    int ver = qr_panic_encode(&pi, "abc", &work);
    ASSERT_EQ(ver, 30);
    ASSERT_TRUE(strstr(work.payload, "more frames cut)\n") != NULL);
    ASSERT_TRUE(strstr(work.payload, "#0 ffffffff80001010 foo+0x10\n") != NULL);   /* the top frame stays */
    ASSERT_TRUE(strstr(work.payload, "http:\\/\\/x \\x01 ") != NULL);
    ASSERT_TRUE(strstr(work.payload, "://") == NULL);
    /* Not even the registers fit: refused, nothing half-built drawn */
    char huge[QR_PANIC_TEXT_MAX];
    memset(huge, 'h', sizeof(huge));
    huge[sizeof(huge) - 1] = '\0';
    pi = make_panic(huge, 4);
    ASSERT_EQ(qr_panic_encode(&pi, "abc", &work), -E2BIG);
    return 0;
}

/* The character drawn in text cell (col, row) at scale 1, white on any
 * darker background, or '\0' if no glyph matches */
static char cell_char(uint32_t col, uint32_t row) {
    for (int ch = 0x20; ch < 0x7F; ch++) {
        int match = 1;
        for (uint32_t gy = 0; gy < FONT_HEIGHT && match; gy++)
            for (uint32_t gx = 0; gx < FONT_WIDTH && match; gx++) {
                int on = (font8x16_data[ch][gy] & (0x80 >> gx)) != 0;
                match = on == (grey_at(col * FONT_WIDTH + gx, row * FONT_HEIGHT + gy) > 128);
            }
        if (match) return (char)ch;
    }
    return '\0';
}

/* Append " <text>" with every run of spaces as one space */
static void squeeze_append(char *out, size_t cap, const char *text) {
    size_t pos = strlen(out);
    for (const char *p = text; *p != '\0' && pos + 2 < cap; p++) {
        if (*p == ' ' && pos > 0 && out[pos - 1] == ' ') continue;
        out[pos++] = *p;
    }
    if (pos > 0 && out[pos - 1] != ' ' && pos + 1 < cap) out[pos++] = ' ';
    out[pos] = '\0';
}

/* A 16-frame panic on a 1024x768 screen: the code is about half the width,
 * and the 101-character register lines must not run under it. They wrap
 * left of the code, so every register and frame stays readable */
TEST(qrscreen_panic_text_beside_code) {
    screen_init(1024, 768, 0);
    PanicInfo pi = make_panic("Page Fault", 16);
    char line[160];
    klog_write(0, "\nPANIC: Page Fault on CPU 3\n", 28);
    for (uint32_t i = 0; i < pi.nregs; i += 4) {
        int n = snprintf(line, sizeof(line), "%-6s %016lx   %-6s %016lx   %-6s %016lx   %-6s %016lx\n", pi.reg_names[i],
                         pi.reg_values[i], pi.reg_names[i + 1], pi.reg_values[i + 1], pi.reg_names[i + 2],
                         pi.reg_values[i + 2], pi.reg_names[i + 3], pi.reg_values[i + 3]);
        ASSERT_EQ(n, 102);   /* 101 columns, as panic_report prints them */
        klog_write(0, line, (size_t)n);
    }
    klog_write(0, "Backtrace:\n", 11);
    for (uint32_t i = 0; i < pi.nframes; i++) {
        int n = snprintf(line, sizeof(line), "  #%-2u %016lx some_function_with_a_long_name+0x%x\n", i, pi.frames[i],
                         0x100 + i);
        klog_write(0, line, (size_t)n);
    }
    klog_write(0, "====\n", 5);
    ASSERT_TRUE(qr_panic_encode(&pi, "0123456789ab", &work) > 0);
    fb_console_panic_view(0);
    ASSERT_EQ(qr_screen_draw_panic(&work.qr), 0);

    uint32_t box = ((uint32_t)work.qr.size + 2 * QR_SCREEN_QUIET) * QR_SCREEN_MIN_SCALE;
    uint32_t text_cols = (1024 - box) / FONT_WIDTH;
    ASSERT_TRUE(text_cols >= FB_PANIC_MIN_COLS && text_cols < 101);   /* the case that wraps */
    long got = decode_screen(1024 - box, FONT_HEIGHT, 1024, FONT_HEIGHT + box, decoded, sizeof(decoded));
    ASSERT_TRUE(got > 0);

    /* Read the text left of the code back off the screen */
    static char seen[16384];
    seen[0] = '\0';
    for (uint32_t row = 1; row < fb_console_rows(); row++) {
        char text[128];
        for (uint32_t c = 0; c < text_cols; c++) text[c] = cell_char(c, row);
        text[text_cols] = '\0';
        ASSERT_TRUE(strlen(text) == text_cols);   /* every cell holds a glyph */
        squeeze_append(seen, sizeof(seen), text);
    }
    for (uint32_t i = 0; i < pi.nregs; i++) {
        char want[64];
        snprintf(want, sizeof(want), " %s %016lx ", pi.reg_names[i], pi.reg_values[i]);
        if (strstr(seen, want) == NULL) {
            printf("  FAIL: register %s not readable on screen\n", pi.reg_names[i]);
            return 1;
        }
    }
    for (uint32_t i = 0; i < pi.nframes; i++) {
        char want[64];
        snprintf(want, sizeof(want), "#%u %016lx ", i, pi.frames[i]);
        if (strstr(seen, want) == NULL) {
            printf("  FAIL: frame #%u not readable on screen\n", i);
            return 1;
        }
    }
    ASSERT_TRUE(strstr(seen, "PANIC: Page Fault on CPU 3 ") != NULL);
    return 0;
}

TEST(qrscreen_panic_small_screen) {
    screen_init(400, 300, 0);
    PanicInfo pi = make_panic("Page Fault", 4);
    ASSERT_TRUE(qr_panic_encode(&pi, "abc", &work) > 0);
    ASSERT_EQ(qr_screen_draw_panic(&work.qr), -ENOSPC);
    return 0;
}

/* ---- Log text ---- */

TEST(qrscreen_log_text) {
    char line[64];
    for (int i = 0; i < 300; i++) {
        int n = snprintf(line, sizeof(line), "\nqrlog line %03d", i);
        klog_write(6, line, (size_t)n);
        klog_write(7, "\nqrlog debug", 12);
    }
    klog_write(6, "\n", 1);
    static char out[8192];
    size_t len = qr_log_text(2048, 6, out, sizeof(out));
    ASSERT_TRUE(len > 0 && len <= 2048);
    out[len] = '\0';
    ASSERT_TRUE(strstr(out, "qrlog line 299\n") != NULL);   /* the newest line */
    ASSERT_TRUE(strstr(out, "qrlog line 100\n") == NULL);   /* outside the window */
    ASSERT_TRUE(strstr(out, "debug") == NULL);              /* above the level */
    ASSERT_TRUE(strchr(out, '<') == NULL);                  /* markers stripped */
    ASSERT_EQ(out[len - 1], '\n');
    /* A small cap stops at a line end */
    size_t small = qr_log_text(2048, 6, out, 40);
    ASSERT_TRUE(small <= 40 && small % 15 == 0);   /* "qrlog line NNN\n" is 15 bytes */
    return 0;
}

TestCase qr_screen_tests[] = {
    TEST_ENTRY(qrscreen_scale),
    TEST_ENTRY(qrscreen_report_pages_decode_32bpp),
    TEST_ENTRY(qrscreen_report_pages_decode_24bpp_bgr),
    TEST_ENTRY(qrscreen_report_pages_uneven_height),
    TEST_ENTRY(qrscreen_page_too_small_screen),
    TEST_ENTRY(qrscreen_panic_text_format),
    TEST_ENTRY(qrscreen_panic_code_decodes),
    TEST_ENTRY(qrscreen_panic_cuts_frames),
    TEST_ENTRY(qrscreen_panic_text_beside_code),
    TEST_ENTRY(qrscreen_panic_small_screen),
    TEST_ENTRY(qrscreen_log_text),
};

int qr_screen_test_count = sizeof(qr_screen_tests) / sizeof(qr_screen_tests[0]);
