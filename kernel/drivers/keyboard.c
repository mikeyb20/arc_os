/* arc_os — PS/2 Keyboard driver implementation
 * Scan code set 1, IRQ 1 handler with shift/ctrl/caps tracking. */

#include "drivers/keyboard.h"
#include "drivers/tty.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"

#define KB_DATA_PORT 0x60

/* Scan code set 1 — unshifted ASCII map (index = scancode) */
static const char scancode_to_ascii[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   /* 0x1D = left ctrl */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   /* 0x2A = left shift */
    '\\','z','x','c','v','b','n','m',',','.','/',
    0,   /* 0x36 = right shift */
    '*', /* keypad */
    0,   /* 0x38 = left alt */
    ' ', /* 0x39 = space */
    0,   /* 0x3A = caps lock */
    /* F1-F10, num lock, scroll lock, etc. — unmapped */
};

/* Scan code set 1 — shifted ASCII map */
static const char scancode_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   /* ctrl */
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   /* left shift */
    '|','Z','X','C','V','B','N','M','<','>','?',
    0,   /* right shift */
    '*',
    0,   /* alt */
    ' ',
    0,   /* caps lock */
};

/* Modifier state */
static volatile int shift_held;
static volatile int ctrl_held;
static volatile int caps_lock;

/* Scancode constants for modifiers */
#define SC_LSHIFT_PRESS   0x2A
#define SC_RSHIFT_PRESS   0x36
#define SC_LSHIFT_RELEASE 0xAA
#define SC_RSHIFT_RELEASE 0xB6
#define SC_CTRL_PRESS     0x1D
#define SC_CTRL_RELEASE   0x9D
#define SC_CAPS_PRESS     0x3A

static void keyboard_irq_handler(InterruptFrame *frame) {
    (void)frame;
    uint8_t scancode = inb(KB_DATA_PORT);

    /* Ignore 0xE0 extended prefix for now */
    if (scancode == 0xE0) return;

    /* Handle modifier key press/release */
    switch (scancode) {
    case SC_LSHIFT_PRESS:
    case SC_RSHIFT_PRESS:
        shift_held = 1;
        return;
    case SC_LSHIFT_RELEASE:
    case SC_RSHIFT_RELEASE:
        shift_held = 0;
        return;
    case SC_CTRL_PRESS:
        ctrl_held = 1;
        return;
    case SC_CTRL_RELEASE:
        ctrl_held = 0;
        return;
    case SC_CAPS_PRESS:
        caps_lock = !caps_lock;
        return;
    }

    /* Ignore key releases (bit 7 set) */
    if (scancode & 0x80) return;

    /* Look up ASCII from scancode table */
    int shifted = shift_held ^ caps_lock;
    char c = shifted ? scancode_shift[scancode] : scancode_to_ascii[scancode];

    /* Ctrl+letter → control code (e.g. Ctrl+C = 0x03) */
    if (ctrl_held && c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 1);
    }

    /* Send non-zero characters to TTY */
    if (c != 0) {
        tty_input_char(c);
    }
}

void keyboard_init(void) {
    shift_held = 0;
    ctrl_held = 0;
    caps_lock = 0;

    /* Register IRQ 1 handler (vector 33) — follows PIT pattern */
    isr_register_handler(IRQ_BASE + 1, keyboard_irq_handler);

    /* Unmask IRQ 1 */
    pic_unmask(1);

    kprintf("[HAL] PS/2 keyboard initialized (IRQ 1, vector %d)\n", IRQ_BASE + 1);
}
