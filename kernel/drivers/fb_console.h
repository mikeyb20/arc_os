#ifndef ARCHOS_DRIVERS_FB_CONSOLE_H
#define ARCHOS_DRIVERS_FB_CONSOLE_H

#include "boot/bootinfo.h"
#include <stddef.h>
#include <stdint.h>

/* Screens at least this big get 2x glyphs (16x32 pixels per character) */
#define FB_SCALE2_MIN_WIDTH  2400
#define FB_SCALE2_MIN_HEIGHT 1350

/* Initialize the framebuffer console with the given framebuffer (24 or 32
 * bits per pixel). Returns 0 on success, -1 if it can't be used. Needs no
 * allocator: until fb_console_attach_cells, scrolling clears the screen. */
int fb_console_init(const Framebuffer *fb);

/* Bytes of cell array the console needs (0 before init). */
size_t fb_console_cells_bytes(void);

/* Give the console its cell array (fb_console_cells_bytes() bytes, kept
 * forever): scrolling then redraws only changed cells and the scrollback view
 * works. Clears the screen. */
void fb_console_attach_cells(void *buf);

/* Text columns and rows (after scaling). */
uint32_t fb_console_cols(void);
uint32_t fb_console_rows(void);

/* Scrollback: move the kernel-log view `pages` screens up (positive) or down
 * (negative). Back at the bottom the live screen returns. The caller holds the
 * console (klog_console_try_acquire). */
void fb_console_scrollback(int pages);

/* Write a single character to the framebuffer console.
 * Handles newline, carriage return, tab, and scrolling. */
void fb_console_putchar(char c);

/* Write a null-terminated string to the framebuffer console. */
void fb_console_puts(const char *s);

/* Clear the entire screen to the background color. */
void fb_console_clear(void);

/* Replace text row `row` with text (cut at the screen width, padded with
 * spaces) in the given colours (0xRRGGBB). The cursor doesn't move. */
void fb_console_write_row(uint32_t row, const char *text, uint32_t fg, uint32_t bg);

/* Set foreground and background colors (0xRRGGBB format). */
void fb_console_set_colors(uint32_t fg, uint32_t bg);

/* Get current cursor position (column, row). */
void fb_console_get_cursor(uint32_t *col, uint32_t *row);

/* Set cursor position. Clamped to screen bounds. */
void fb_console_set_cursor(uint32_t col, uint32_t row);

/* Erase display: 0=cursor to end, 1=start to cursor, 2=entire screen */
void fb_console_erase_display(int mode);

/* Erase line: 0=cursor to end, 1=start to cursor, 2=entire line */
void fb_console_erase_line(int mode);

/* Narrowest text column the panic view wraps to (below it, full width) */
#define FB_PANIC_MIN_COLS 40

/* Panic screen (called by the panic screen hook, diag/qr_mode.c): the end
 * of the kernel log, white on dark red under a yellow banner; the screen then
 * stays frozen. With text_width > 0, lines wrap (at a space where there is
 * one) to the whole columns that fit in that many pixels from the left edge,
 * so a QR code drawn right of them covers no text; 0, or a width under
 * FB_PANIC_MIN_COLS columns, uses the full width. May be called again to
 * redraw. Takes no lock (panic path). Cost: two walks of the log ring. */
void fb_console_panic_view(uint32_t text_width);

/* ---- Drawing outside the text grid (QR screens, M1-04c) ---- */

/* Screen size in pixels and the height of one text row (0 before init). */
uint32_t fb_console_width(void);
uint32_t fb_console_height(void);
uint32_t fb_console_row_height(void);

/* Pause: from now on output updates the cell array only, so the caller owns
 * the screen; resume redraws the text screen from the cells (or clears it
 * when there are none). The caller holds the console
 * (klog_console_acquire) for each call and for every draw below. */
void fb_console_pause(void);
void fb_console_resume(void);

/* Fill the whole screen with one colour (0xRRGGBB). Pixels only. */
void fb_console_fill(uint32_t rgb);

/* Draw text on row `row` (cut at the screen width, padded with spaces) in
 * the given colours. Pixels only: the cells and the cursor don't change. */
void fb_console_paint_row(uint32_t row, const char *text, uint32_t fg, uint32_t bg);

/* Draw a 1-bit bitmap, w x h modules, 1 = dark: module (mx, my) is bit
 * 7 - mx % 8 of bits[my * row_bytes + mx / 8]. Each module is scale x scale
 * pixels, black for 1 and white for 0, with a white border `quiet` modules
 * wide on every side; the border's top-left pixel is (x, y). Pixels outside
 * the screen are skipped. Pixels only, written once each, never read. */
void fb_console_draw_bitmap(const uint8_t *bits, uint32_t w, uint32_t h, uint32_t row_bytes, uint32_t quiet,
                            uint32_t scale, uint32_t x, uint32_t y);

#endif /* ARCHOS_DRIVERS_FB_CONSOLE_H */
