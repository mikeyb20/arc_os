/* arc_os — TTY subsystem
 * Bridges keyboard IRQ input and user-space read/write via ring buffer. */

#ifndef ARCHOS_DRIVERS_TTY_H
#define ARCHOS_DRIVERS_TTY_H

#include <stdint.h>

/* Initialize the TTY subsystem (call before keyboard_init) */
void tty_init(void);

/* Deposit a character from keyboard IRQ context */
void tty_input_char(char c);

/* Read up to count bytes from TTY; blocks (yield-loops) until a line is ready */
int tty_read(void *buf, uint32_t count);

/* Write count bytes to serial output */
int tty_write(const void *buf, uint32_t count);

/* Set the foreground process PID for Ctrl+C delivery */
void tty_set_fg_pid(uint32_t pid);

/* Set the foreground process group for signal delivery */
void tty_set_fg_pgid(uint32_t pgid);

/* Foreground process group of the terminal (0 = none). TIOCGPGRP reads it. */
uint32_t tty_get_fg_pgid(void);

struct termios;
struct winsize;

/* Terminal settings for TCGETS/TCSETS*. Stored and reported only: input is
 * always canonical with echo until the terminal block (M5) applies them.
 * Defaults are Linux's (ICANON, ECHO, ISIG, ICRNL, OPOST|ONLCR, B38400 CS8). */
void tty_get_termios(struct termios *t);
void tty_set_termios(const struct termios *t);

/* Drop typed input not yet read (TCSETSF). */
void tty_flush_input(void);

/* Window size for TIOCGWINSZ/TIOCSWINSZ (musl's isatty() asks for it).
 * Defaults to 25 x 80; kmain sets the framebuffer console's size. */
void tty_get_winsize(struct winsize *ws);
void tty_set_winsize(const struct winsize *ws);

/* Set the output function for tty (e.g., serial + framebuffer) */
void tty_set_output(void (*fn)(char c));

#endif /* ARCHOS_DRIVERS_TTY_H */
