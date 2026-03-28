#include "drivers/fb_console.h"
#include "drivers/font8x16.h"
#include "lib/mem.h"

/* Console state */
static uint8_t *fb_addr;       /* Hardware framebuffer */
static uint8_t *backbuf;       /* Back buffer (if double-buffering) */
static uint8_t *render_target; /* Where rendering goes (backbuf or fb_addr) */
static uint64_t fb_width;
static uint64_t fb_height;
static uint64_t fb_pitch;
static uint16_t fb_bpp;
static uint8_t  fb_red_shift;
static uint8_t  fb_green_shift;
static uint8_t  fb_blue_shift;

static uint32_t cols;    /* Characters per row */
static uint32_t rows;    /* Character rows */
static uint32_t cur_col; /* Current column */
static uint32_t cur_row; /* Current row */

static uint32_t fg_color; /* Packed foreground pixel */
static uint32_t bg_color; /* Packed background pixel */

static int fb_initialized;
static int fb_dirty;       /* Dirty flag for flush optimization */

/* Pack an RGB color into the framebuffer's native pixel format. */
static uint32_t pack_color(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return ((uint32_t)r << fb_red_shift) |
           ((uint32_t)g << fb_green_shift) |
           ((uint32_t)b << fb_blue_shift);
}

/* Write a pixel at (x, y) to the render target. */
static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    uint32_t bytes_per_pixel = fb_bpp / 8;
    uint32_t *pixel = (uint32_t *)(render_target + y * fb_pitch + x * bytes_per_pixel);
    *pixel = color;
}

/* Render a single glyph at character position (col, row). */
static void render_glyph(uint32_t col, uint32_t row, uint8_t ch) {
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;
    const uint8_t *glyph = font8x16_data[ch & 0x7F];

    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = glyph[gy];
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t color = (bits & (0x80 >> gx)) ? fg_color : bg_color;
            put_pixel(px + gx, py + gy, color);
        }
    }
    fb_dirty = 1;
}

/* Clear one character cell to background. */
static void clear_cell(uint32_t col, uint32_t row) {
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            put_pixel(px + gx, py + gy, bg_color);
        }
    }
    fb_dirty = 1;
}

/* Scroll the screen up by one text row. */
static void scroll_up(void) {
    uint64_t row_bytes = fb_pitch * FONT_HEIGHT;
    uint8_t *dst = render_target;
    uint8_t *src = render_target + row_bytes;
    uint64_t copy_size = row_bytes * (rows - 1);
    memmove(dst, src, copy_size);

    /* Clear the last row */
    uint8_t *last_row = render_target + row_bytes * (rows - 1);
    memset(last_row, 0, row_bytes);
    fb_dirty = 1;
}

/* Advance to the next line, scrolling if necessary. */
static void newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= rows) {
        scroll_up();
        cur_row = rows - 1;
    }
}

int fb_console_init(const Framebuffer *fb) {
    if (fb == NULL || fb->address == NULL || fb->bpp < 24) {
        return -1;
    }

    fb_addr = (uint8_t *)fb->address;
    fb_width = fb->width;
    fb_height = fb->height;
    fb_pitch = fb->pitch;
    fb_bpp = fb->bpp;
    fb_red_shift = fb->red_mask_shift;
    fb_green_shift = fb->green_mask_shift;
    fb_blue_shift = fb->blue_mask_shift;

    cols = (uint32_t)(fb_width / FONT_WIDTH);
    rows = (uint32_t)(fb_height / FONT_HEIGHT);
    cur_col = 0;
    cur_row = 0;

    /* Default: light gray on black */
    fg_color = pack_color(0xAAAAAA);
    bg_color = pack_color(0x000000);

    /* No backbuffer yet — render directly to hardware.
     * Double-buffering can be enabled later after PMM is available. */
    backbuf = NULL;
    render_target = fb_addr;

    fb_initialized = 1;
    fb_dirty = 0;

    fb_console_clear();
    return 0;
}

void fb_console_putchar(char c) {
    if (!fb_initialized) return;

    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cur_col = 0;
        break;
    case '\t':
        /* Advance to next 8-column tab stop */
        cur_col = (cur_col + 8) & ~7u;
        if (cur_col >= cols) {
            newline();
        }
        break;
    case '\b':
        if (cur_col > 0) {
            cur_col--;
            clear_cell(cur_col, cur_row);
        }
        break;
    default:
        if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
            render_glyph(cur_col, cur_row, (uint8_t)c);
            cur_col++;
            if (cur_col >= cols) {
                newline();
            }
        }
        break;
    }
}

void fb_console_puts(const char *s) {
    while (*s) {
        fb_console_putchar(*s++);
    }
}

void fb_console_clear(void) {
    if (!fb_initialized) return;
    memset(render_target, 0, fb_pitch * fb_height);
    cur_col = 0;
    cur_row = 0;
    fb_dirty = 1;
}

void fb_console_set_colors(uint32_t fg, uint32_t bg) {
    fg_color = pack_color(fg);
    bg_color = pack_color(bg);
}

void fb_console_get_cursor(uint32_t *col, uint32_t *row) {
    if (col) *col = cur_col;
    if (row) *row = cur_row;
}

void fb_console_set_cursor(uint32_t col, uint32_t row) {
    if (col >= cols) col = cols - 1;
    if (row >= rows) row = rows - 1;
    cur_col = col;
    cur_row = row;
}

void fb_console_erase_display(int mode) {
    if (!fb_initialized) return;

    if (mode == 2) {
        /* Erase entire screen */
        memset(render_target, 0, fb_pitch * fb_height);
        cur_col = 0;
        cur_row = 0;
    } else if (mode == 0) {
        /* Erase from cursor to end */
        for (uint32_t c = cur_col; c < cols; c++) clear_cell(c, cur_row);
        for (uint32_t r = cur_row + 1; r < rows; r++)
            for (uint32_t c = 0; c < cols; c++) clear_cell(c, r);
    } else if (mode == 1) {
        /* Erase from start to cursor */
        for (uint32_t r = 0; r < cur_row; r++)
            for (uint32_t c = 0; c < cols; c++) clear_cell(c, r);
        for (uint32_t c = 0; c <= cur_col; c++) clear_cell(c, cur_row);
    }
    fb_dirty = 1;
}

void fb_console_erase_line(int mode) {
    if (!fb_initialized) return;

    if (mode == 2) {
        /* Erase entire line */
        for (uint32_t c = 0; c < cols; c++) clear_cell(c, cur_row);
    } else if (mode == 0) {
        /* Erase from cursor to end of line */
        for (uint32_t c = cur_col; c < cols; c++) clear_cell(c, cur_row);
    } else if (mode == 1) {
        /* Erase from start of line to cursor */
        for (uint32_t c = 0; c <= cur_col; c++) clear_cell(c, cur_row);
    }
    fb_dirty = 1;
}

void fb_console_flush(void) {
    if (!fb_initialized || !backbuf || !fb_dirty) return;
    memcpy(fb_addr, backbuf, fb_pitch * fb_height);
    fb_dirty = 0;
}
