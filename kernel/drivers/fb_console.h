#ifndef ARCHOS_DRIVERS_FB_CONSOLE_H
#define ARCHOS_DRIVERS_FB_CONSOLE_H

#include "boot/bootinfo.h"
#include <stdint.h>

/* Initialize the framebuffer console with the given framebuffer.
 * Returns 0 on success, -1 if framebuffer is not available. */
int fb_console_init(const Framebuffer *fb);

/* Write a single character to the framebuffer console.
 * Handles newline, carriage return, tab, and scrolling. */
void fb_console_putchar(char c);

/* Write a null-terminated string to the framebuffer console. */
void fb_console_puts(const char *s);

/* Clear the entire screen to the background color. */
void fb_console_clear(void);

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

/* Flush backbuffer to hardware framebuffer (when double-buffering is active) */
void fb_console_flush(void);

#endif /* ARCHOS_DRIVERS_FB_CONSOLE_H */
