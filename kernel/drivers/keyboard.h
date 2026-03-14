/* arc_os — PS/2 Keyboard driver
 * Handles IRQ 1, translates scan code set 1 to ASCII. */

#ifndef ARCHOS_DRIVERS_KEYBOARD_H
#define ARCHOS_DRIVERS_KEYBOARD_H

/* Initialize PS/2 keyboard: register IRQ 1 handler, unmask IRQ 1 */
void keyboard_init(void);

#endif /* ARCHOS_DRIVERS_KEYBOARD_H */
