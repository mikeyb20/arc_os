#ifndef ARCHOS_DRIVERS_ANSI_H
#define ARCHOS_DRIVERS_ANSI_H

#include <stdint.h>

/* Initialize the ANSI parser state */
void ansi_init(void);

/* Process one character through the ANSI state machine.
 * Printable characters go to the framebuffer; escape sequences
 * are interpreted and translated into fb_console operations. */
void ansi_putchar(char c);

/* Reset parser to ground state */
void ansi_reset(void);

#endif /* ARCHOS_DRIVERS_ANSI_H */
