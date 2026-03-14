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

#endif /* ARCHOS_DRIVERS_TTY_H */
