/* arc_os — PS/2 Keyboard driver
 * Handles IRQ 1, translates scan code set 1 to ASCII. */

#ifndef ARCHOS_DRIVERS_KEYBOARD_H
#define ARCHOS_DRIVERS_KEYBOARD_H

#include <stdint.h>

/* Initialize PS/2 keyboard: register IRQ 1 handler, unmask IRQ 1 */
void keyboard_init(void);

/* Key presses since boot (modifiers not counted). */
uint32_t keyboard_key_count(void);

/* on = 1: keys are only counted, not passed to the TTY (boot report pager). */
void keyboard_grab(int on);

#endif /* ARCHOS_DRIVERS_KEYBOARD_H */
