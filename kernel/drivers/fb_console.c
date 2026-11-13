/* arc_os — Framebuffer text console
 *
 * Owns: drawing text on the boot framebuffer (8x16 font, scaled 2x on large
 * screens so photos of it stay readable), the cursor, and the kernel-log
 * scrollback view (M1-04).
 * Invariants: pixels are only ever written, never read back: on real hardware
 * the framebuffer is uncached or write-combined, and reading it (the old
 * memmove scroll) costs tens of milliseconds per line. Once kmain attaches a
 * cell array (fb_console_attach_cells), it holds what every character cell
 * shows, and scrolling redraws only the cells that change. While the
 * scrollback view is open, the console is paused (QR screens, M1-04c) or
 * the panic screen is up, output updates the cells but not the screen.
 * Pixel colours are packed with the framebuffer's own mask sizes and shifts.
 * Locks: none of its own. kprintf output arrives through the log drain, which
 * has one owner at a time (lib/klog.c); the scrollback keys take that owner
 * too, and so do TTY output, VT switches (klog_console_acquire, M1-05) and
 * the QR screens' pause, draw and resume. The panic screen takes nothing.
 * Cost: a glyph is 128 pixel writes (512 at 2x); a full-screen fill or bitmap
 * writes each pixel once; scrollback and the panic view walk the log ring. */

#include "drivers/fb_console.h"
#include "drivers/font8x16.h"
#include "lib/klog.h"
#include "lib/kprintf.h"
#include "lib/mem.h"

/* One character cell as drawn: character and packed pixel colours */
typedef struct {
    uint32_t fg;
    uint32_t bg;
    uint8_t ch;
} FbCell;

static uint8_t *fb_addr;
static uint64_t fb_width;
static uint64_t fb_height;
static uint64_t fb_pitch;
static uint16_t fb_bpp;
static uint8_t fb_mask_size[3];   /* red, green, blue */
static uint8_t fb_mask_shift[3];

static uint32_t scale;   /* glyph pixel -> scale x scale screen pixels */
static uint32_t cols;    /* characters per row */
static uint32_t rows;    /* character rows */
static uint32_t cur_col;
static uint32_t cur_row;

static uint32_t fg_color; /* packed foreground pixel */
static uint32_t bg_color; /* packed background pixel */

static FbCell *cells;    /* rows * cols, or NULL before kmain attaches it */
static uint32_t view_pages;   /* scrollback: pages above the live screen, 0 = live */
static int paused;            /* fb_console_pause: the screen belongs to its caller */

static int fb_initialized;

/* Output reaches the screen (not covered by scrollback, a pause or a panic). */
static inline int screen_live(void) {
    return view_pages == 0 && !paused;
}

/* Scale one 8-bit channel to a mask of `size` bits. */
static uint32_t channel(uint32_t v8, int i) {
    uint32_t size = fb_mask_size[i] ? fb_mask_size[i] : 8;
    uint32_t v = size >= 8 ? v8 << (size - 8) : v8 >> (8 - size);
    return v << fb_mask_shift[i];
}

/* Pack an RGB colour (0xRRGGBB) into the framebuffer's pixel format. */
static uint32_t pack_color(uint32_t rgb) {
    return channel((rgb >> 16) & 0xFF, 0) | channel((rgb >> 8) & 0xFF, 1) | channel(rgb & 0xFF, 2);
}

/* Write one pixel (32 or 24 bits per pixel). */
static inline void put_pixel(uint64_t x, uint64_t y, uint32_t color) {
    uint8_t *p = fb_addr + y * fb_pitch + x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        *(volatile uint32_t *)p = color;
    } else {
        p[0] = (uint8_t)color;
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
    }
}

/* Draw character ch at cell (col, row) without touching the cell array. */
static void draw_glyph(uint32_t col, uint32_t row, uint8_t ch, uint32_t fg, uint32_t bg) {
    if (col >= cols || row >= rows) return;
    const uint8_t *glyph = font8x16_data[ch & 0x7F];
    uint64_t px = (uint64_t)col * FONT_WIDTH * scale;
    uint64_t py = (uint64_t)row * FONT_HEIGHT * scale;
    for (uint32_t gy = 0; gy < FONT_HEIGHT * scale; gy++) {
        uint8_t bits = glyph[gy / scale];
        for (uint32_t gx = 0; gx < FONT_WIDTH * scale; gx++) {
            put_pixel(px + gx, py + gy, (bits & (0x80 >> (gx / scale))) ? fg : bg);
        }
    }
}

/* Set cell (col, row) and draw it unless the scrollback view covers the screen. */
static void set_cell(uint32_t col, uint32_t row, uint8_t ch, uint32_t fg, uint32_t bg) {
    if (col >= cols || row >= rows) return;
    if (cells != NULL) {
        FbCell *c = &cells[row * cols + col];
        c->ch = ch;
        c->fg = fg;
        c->bg = bg;
    }
    if (screen_live()) draw_glyph(col, row, ch, fg, bg);
}

/* Fill the whole screen with colour (writes only). */
static void fill_screen(uint32_t color) {
    for (uint64_t y = 0; y < fb_height; y++) {
        for (uint64_t x = 0; x < fb_width; x++) put_pixel(x, y, color);
    }
}

/* Draw every cell from the cell array (leaving the scrollback view). */
static void redraw_all(void) {
    if (cells == NULL) return;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            FbCell *cell = &cells[r * cols + c];
            draw_glyph(c, r, cell->ch, cell->fg, cell->bg);
        }
    }
}

/* Scroll up one text row. With the cell array, redraw only cells whose
 * content changes; without it (very early boot), clear the screen and start
 * at the top, since reading the framebuffer back is far too slow. */
static void scroll_up(void) {
    if (cells == NULL) {
        if (screen_live()) fill_screen(bg_color);
        cur_row = 0;
        return;
    }
    for (uint32_t r = 0; r + 1 < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            FbCell *dst = &cells[r * cols + c];
            const FbCell *src = &cells[(r + 1) * cols + c];
            if (dst->ch != src->ch || dst->fg != src->fg || dst->bg != src->bg) {
                *dst = *src;
                if (screen_live()) draw_glyph(c, r, dst->ch, dst->fg, dst->bg);
            }
        }
    }
    for (uint32_t c = 0; c < cols; c++) set_cell(c, rows - 1, ' ', fg_color, bg_color);
    cur_row = rows - 1;
}

/* Advance to the next line, scrolling if necessary. */
static void newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= rows) scroll_up();
}

int fb_console_init(const Framebuffer *fb) {
    if (fb == NULL || fb->address == NULL || (fb->bpp != 24 && fb->bpp != 32)) {
        return -1;
    }

    fb_addr = (uint8_t *)fb->address;
    fb_width = fb->width;
    fb_height = fb->height;
    fb_pitch = fb->pitch;
    fb_bpp = fb->bpp;
    fb_mask_size[0] = fb->red_mask_size;
    fb_mask_size[1] = fb->green_mask_size;
    fb_mask_size[2] = fb->blue_mask_size;
    fb_mask_shift[0] = fb->red_mask_shift;
    fb_mask_shift[1] = fb->green_mask_shift;
    fb_mask_shift[2] = fb->blue_mask_shift;

    /* 2x glyphs from 2400x1350 up: a 2.8K laptop panel gets 180 columns */
    scale = (fb_width >= FB_SCALE2_MIN_WIDTH && fb_height >= FB_SCALE2_MIN_HEIGHT) ? 2 : 1;
    cols = (uint32_t)(fb_width / (FONT_WIDTH * scale));
    rows = (uint32_t)(fb_height / (FONT_HEIGHT * scale));
    cur_col = 0;
    cur_row = 0;
    cells = NULL;
    view_pages = 0;
    paused = 0;

    /* Default: light gray on black */
    fg_color = pack_color(0xAAAAAA);
    bg_color = pack_color(0x000000);

    fb_initialized = 1;
    fb_console_clear();
    return 0;
}

size_t fb_console_cells_bytes(void) {
    return fb_initialized ? (size_t)cols * rows * sizeof(FbCell) : 0;
}

void fb_console_attach_cells(void *buf) {
    if (!fb_initialized || buf == NULL) return;
    cells = (FbCell *)buf;
    /* Start from a blank screen so the cells match what is shown */
    for (uint32_t i = 0; i < cols * rows; i++) {
        cells[i].ch = ' ';
        cells[i].fg = fg_color;
        cells[i].bg = bg_color;
    }
    fill_screen(bg_color);
    cur_col = 0;
    cur_row = 0;
}

uint32_t fb_console_cols(void) {
    return cols;
}

uint32_t fb_console_rows(void) {
    return rows;
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
            set_cell(cur_col, cur_row, ' ', fg_color, bg_color);
        }
        break;
    default:
        if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
            set_cell(cur_col, cur_row, (uint8_t)c, fg_color, bg_color);
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
    if (cells != NULL) {
        for (uint32_t i = 0; i < cols * rows; i++) {
            cells[i].ch = ' ';
            cells[i].fg = fg_color;
            cells[i].bg = bg_color;
        }
    }
    if (screen_live()) fill_screen(bg_color);
    cur_col = 0;
    cur_row = 0;
}

void fb_console_write_row(uint32_t row, const char *text, uint32_t fg, uint32_t bg) {
    if (!fb_initialized || row >= rows) return;
    uint32_t pfg = pack_color(fg), pbg = pack_color(bg);
    uint32_t c = 0;
    for (; c < cols && text[c] != '\0'; c++) {
        uint8_t ch = ((uint8_t)text[c] >= 0x20 && (uint8_t)text[c] < 0x7F) ? (uint8_t)text[c] : '?';
        set_cell(c, row, ch, pfg, pbg);
    }
    for (; c < cols; c++) set_cell(c, row, ' ', pfg, pbg);
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
        fb_console_clear();
    } else if (mode == 0) {
        /* Erase from cursor to end */
        for (uint32_t c = cur_col; c < cols; c++) set_cell(c, cur_row, ' ', fg_color, bg_color);
        for (uint32_t r = cur_row + 1; r < rows; r++)
            for (uint32_t c = 0; c < cols; c++) set_cell(c, r, ' ', fg_color, bg_color);
    } else if (mode == 1) {
        /* Erase from start to cursor */
        for (uint32_t r = 0; r < cur_row; r++)
            for (uint32_t c = 0; c < cols; c++) set_cell(c, r, ' ', fg_color, bg_color);
        for (uint32_t c = 0; c <= cur_col; c++) set_cell(c, cur_row, ' ', fg_color, bg_color);
    }
}

void fb_console_erase_line(int mode) {
    if (!fb_initialized) return;

    uint32_t from = (mode == 0) ? cur_col : 0;
    uint32_t to = (mode == 1) ? cur_col + 1 : cols;
    for (uint32_t c = from; c < to && c < cols; c++) set_cell(c, cur_row, ' ', fg_color, bg_color);
}

/* ---- Kernel-log scrollback (M1-04) ---- */

/* Walk the printable log lines (level at or below the console level, from
 * the oldest line still in the ring). Calls fn(line_index, text, len, ctx) for
 * each line whose index is in [first, first + count); returns the number of
 * lines. */
typedef void (*line_fn)(uint32_t index, const char *text, uint32_t len, void *ctx);

typedef struct {
    uint32_t first, count, index;
    line_fn fn;
    void *ctx;
} LineWalk;

static void count_line(const char *text, size_t len, void *ctx) {
    LineWalk *w = ctx;
    if (w->fn != NULL && w->index >= w->first && w->index - w->first < w->count)
        w->fn(w->index, text, (uint32_t)len, w->ctx);
    w->index++;
}

static uint32_t log_lines(uint32_t first, uint32_t count, line_fn fn, void *ctx) {
    LineWalk w = {.first = first, .count = count, .fn = fn, .ctx = ctx};
    klog_for_each_line(klog_tail(), klog_console_level(), count_line, &w);
    return w.index;
}

/* Where log lines go: line `first` on row 1 (row 0 is the banner) */
typedef struct {
    uint32_t first;
    uint32_t fg, bg;   /* packed */
} LogView;

/* Draw text[0..len) on row `row`, padded with spaces to the full width, with
 * control bytes as spaces (pixels only). */
static void draw_row(uint32_t row, const char *text, uint32_t len, uint32_t fg, uint32_t bg) {
    for (uint32_t c = 0; c < cols; c++) {
        uint8_t ch = (c < len && (uint8_t)text[c] >= 0x20 && (uint8_t)text[c] < 0x7F) ? (uint8_t)text[c] : ' ';
        draw_glyph(c, row, ch, fg, bg);
    }
}

static void draw_log_line(uint32_t index, const char *text, uint32_t len, void *ctx) {
    const LogView *v = ctx;
    draw_row(1 + index - v->first, text, len, v->fg, v->bg);
}

/* Fill rows 1.. with log lines [first, end), then blank the rows below them
 * (each pixel is written once). */
static void draw_log_page(uint32_t first, uint32_t end, uint32_t fg, uint32_t bg) {
    LogView v = {.first = first, .fg = fg, .bg = bg};
    uint32_t total = log_lines(first, end - first, draw_log_line, &v);
    uint32_t last = total < end ? total : end;
    for (uint32_t r = 1 + (last > first ? last - first : 0); r < rows; r++) draw_log_line(first + r - 1, "", 0, &v);
}

/* Draw text on row `row` in packed colours, padded with spaces (pixels only). */
static void draw_text_row(uint32_t row, const char *text, uint32_t fg, uint32_t bg) {
    uint32_t c = 0;
    for (; text[c] != '\0' && c < cols; c++) {
        uint8_t ch = ((uint8_t)text[c] >= 0x20 && (uint8_t)text[c] < 0x7F) ? (uint8_t)text[c] : '?';
        draw_glyph(c, row, ch, fg, bg);
    }
    for (; c < cols; c++) draw_glyph(c, row, ' ', fg, bg);
}

void fb_console_scrollback(int pages) {
    if (!fb_initialized || cells == NULL || rows < 2 || paused) return;

    uint32_t page = rows - 1;   /* log lines per page, under the banner */
    uint32_t total = log_lines(0, 0, NULL, NULL);
    uint32_t max_pages = total > page ? (total - 1) / page : 0;

    int64_t want = (int64_t)view_pages + pages;
    if (want < 0) want = 0;
    if (want > (int64_t)max_pages) want = (int64_t)max_pages;
    if ((uint32_t)want == view_pages) return;
    view_pages = (uint32_t)want;

    if (view_pages == 0) {
        redraw_all();   /* back to the live screen */
        return;
    }
    /* The page that ends view_pages pages above the newest line */
    uint32_t end = total - view_pages * page;
    uint32_t first = end > page ? end - page : 0;
    char banner[96];
    ksnprintf(banner, sizeof(banner), " Kernel log: lines %u-%u of %u  (Shift+PgUp/PgDn)", first + 1, end, total);
    draw_text_row(0, banner, pack_color(0x000000), pack_color(0xFFD75F));
    draw_log_page(first, end, pack_color(0xAAAAAA), pack_color(0x000000));
}

/* Length of the next screen row of a wrapped line text[0..len), at most width
 * columns: a longer line breaks at its last space within the row (or at the
 * width when there is none). *used is how many bytes the row consumes, the
 * spaces at the break included. */
static uint32_t wrap_row(const char *text, uint32_t len, uint32_t width, uint32_t *used) {
    if (len <= width) {
        *used = len;
        return len;
    }
    for (uint32_t c = width; c > 0; c--) {
        if (text[c] == ' ') {
            uint32_t u = c + 1;
            while (u < len && text[u] == ' ') u++;   /* the next row starts at text, not spaces */
            *used = u;
            return c;
        }
    }
    *used = width;
    return width;
}

/* The panic view's walk: the log's lines wrapped at `width` columns, screen
 * rows numbered from the oldest; rows [first, first + rows - 1) are drawn */
typedef struct {
    uint32_t width, first, index;
    int draw;
} WrapView;

static void wrap_line(const char *text, size_t len, void *ctx) {
    WrapView *v = ctx;
    uint32_t pos = 0, n = len < KLOG_LINE_MAX ? (uint32_t)len : KLOG_LINE_MAX;
    do {
        uint32_t used, seg = wrap_row(text + pos, n - pos, v->width, &used);
        if (v->draw && v->index >= v->first && v->index - v->first < rows - 1)
            draw_row(1 + v->index - v->first, text + pos, seg, pack_color(0xFFFFFF), pack_color(0x8B0000));
        v->index++;
        pos += used;
    } while (pos < n);
}

void fb_console_panic_view(uint32_t text_width) {
    if (!fb_initialized || rows < 2) return;
    view_pages = UINT32_MAX;   /* freeze: later output updates cells only */
    uint32_t width = text_width / (FONT_WIDTH * scale);
    if (text_width == 0 || width >= cols || width < FB_PANIC_MIN_COLS) width = cols;
    WrapView v = {.width = width};
    klog_for_each_line(klog_tail(), klog_console_level(), wrap_line, &v);   /* count the rows */
    uint32_t total = v.index;
    v.first = total > rows - 1 ? total - (rows - 1) : 0;
    v.index = 0;
    v.draw = 1;
    draw_text_row(0, " arc_os KERNEL PANIC - scan the code or photograph this screen", pack_color(0x000000),
                  pack_color(0xFFFF00));
    klog_for_each_line(klog_tail(), klog_console_level(), wrap_line, &v);
    /* Lines written between the two walks may have moved the window: blank what is left */
    uint32_t drawn = v.index > v.first ? v.index - v.first : 0;
    for (uint32_t r = 1 + (drawn < rows - 1 ? drawn : rows - 1); r < rows; r++)
        draw_row(r, "", 0, pack_color(0xFFFFFF), pack_color(0x8B0000));
}

/* ---- Drawing outside the text grid (QR screens, M1-04c) ---- */

uint32_t fb_console_width(void) {
    return fb_initialized ? (uint32_t)fb_width : 0;
}

uint32_t fb_console_height(void) {
    return fb_initialized ? (uint32_t)fb_height : 0;
}

uint32_t fb_console_row_height(void) {
    return fb_initialized ? FONT_HEIGHT * scale : 0;
}

void fb_console_pause(void) {
    paused = 1;
}

void fb_console_resume(void) {
    if (!paused) return;
    paused = 0;
    if (!fb_initialized || view_pages != 0) return;
    if (cells != NULL)
        redraw_all();
    else
        fill_screen(bg_color);
}

void fb_console_fill(uint32_t rgb) {
    if (fb_initialized) fill_screen(pack_color(rgb));
}

void fb_console_paint_row(uint32_t row, const char *text, uint32_t fg, uint32_t bg) {
    if (fb_initialized && row < rows) draw_text_row(row, text, pack_color(fg), pack_color(bg));
}

void fb_console_draw_bitmap(const uint8_t *bits, uint32_t w, uint32_t h, uint32_t row_bytes, uint32_t quiet,
                            uint32_t scale_px, uint32_t x, uint32_t y) {
    if (!fb_initialized || scale_px == 0) return;
    uint32_t dark = pack_color(0x000000), light = pack_color(0xFFFFFF);
    uint64_t total_w = (uint64_t)(w + 2 * quiet) * scale_px;
    uint64_t total_h = (uint64_t)(h + 2 * quiet) * scale_px;
    for (uint64_t py = 0; py < total_h && y + py < fb_height; py++) {
        int64_t my = (int64_t)(py / scale_px) - quiet;
        for (uint64_t px = 0; px < total_w && x + px < fb_width; px++) {
            int64_t mx = (int64_t)(px / scale_px) - quiet;
            int on = mx >= 0 && my >= 0 && mx < (int64_t)w && my < (int64_t)h &&
                     ((bits[(uint64_t)my * row_bytes + (uint64_t)mx / 8] >> (7 - mx % 8)) & 1);
            put_pixel(x + px, y + py, on ? dark : light);
        }
    }
}
