#ifndef ARCHOS_DRIVERS_FB_CONSOLE_H
#define ARCHOS_DRIVERS_FB_CONSOLE_H

#include "boot/bootinfo.h"
#include <stddef.h>
#include <stdint.h>

/* Screens at least this big get 2x glyphs (16x32 pixels per character) */
#define FB_SCALE2_MIN_WIDTH  2400
#define FB_SCALE2_MIN_HEIGHT 1350

/* Longest log line the scrollback view keeps (the rest is cut) */
#define FB_MAX_LINE 256

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

/* Panic screen (lib/panic.h panic_set_screen): the end of the kernel log,
 * white on dark red under a yellow banner; the screen then stays frozen. */
void fb_console_panic_view(void);

#endif /* ARCHOS_DRIVERS_FB_CONSOLE_H */
