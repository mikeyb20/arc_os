/* arc_os — PS/2 Keyboard driver implementation
 * Scan code set 1, IRQ 1 handler with shift/ctrl/alt/caps tracking.
 * Alt+F1-F6 switches virtual terminals. */

#include "drivers/keyboard.h"
#include "drivers/tty.h"
#include "drivers/vt.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/io.h"
#include "lib/kprintf.h"
#include "lib/klog.h"
#include "drivers/fb_console.h"
#include "proc/kernel_lock.h"

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
static volatile int alt_held;
static volatile int caps_lock;
static volatile int extended;   /* the last byte was the E0 prefix */
static volatile uint32_t key_count;   /* key presses since boot (M1-04 pager) */
static volatile int grabbed;          /* keys are counted, not sent to the TTY */

/* Scancode constants for modifiers */
#define SC_LSHIFT_PRESS   0x2A
#define SC_RSHIFT_PRESS   0x36
#define SC_LSHIFT_RELEASE 0xAA
#define SC_RSHIFT_RELEASE 0xB6
#define SC_CTRL_PRESS     0x1D
#define SC_CTRL_RELEASE   0x9D
#define SC_ALT_PRESS      0x38
#define SC_ALT_RELEASE    0xB8
#define SC_CAPS_PRESS     0x3A
#define SC_EXTENDED_PREFIX  0xE0
#define SC_RELEASE_BIT      0x80
#define SC_EXT_PGUP         0x49   /* after E0 */
#define SC_EXT_PGDN         0x51

/* F1-F6 scancodes */
#define SC_F1  0x3B
#define SC_F2  0x3C
#define SC_F3  0x3D
#define SC_F4  0x3E
#define SC_F5  0x3F
#define SC_F6  0x40

/* Shift+PgUp / Shift+PgDn: scroll the kernel log on screen (M1-04). Needs the
 * console, like the log drain; if another context is printing, the key is
 * dropped. */
static void keyboard_scrollback(int pages) {
    if (klog_console_try_acquire()) {
        fb_console_scrollback(pages);
        klog_console_release();
    }
}

static void keyboard_scancode(uint8_t scancode);

/* IRQ 1. Under the kernel lock: the TTY and signal paths it feeds have no
 * locks of their own yet (M1-05). */
static void keyboard_irq_handler(InterruptFrame *frame) {
    (void)frame;
    uint8_t scancode = inb(KB_DATA_PORT);
    kernel_lock_irq();
    keyboard_scancode(scancode);
    kernel_unlock();
}

static void keyboard_scancode(uint8_t scancode) {

    /* Extended keys (E0 xx) other than these are handled like their plain
     * twins, as before (right Ctrl, right Alt, keypad Enter) */
    if (scancode == SC_EXTENDED_PREFIX) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        uint8_t key = scancode & (uint8_t)~SC_RELEASE_BIT;
        /* E0 2A / E0 36 are fake shifts keyboards send around PgUp/PgDn */
        if (key == SC_LSHIFT_PRESS || key == SC_RSHIFT_PRESS) return;
        if (key == SC_EXT_PGUP || key == SC_EXT_PGDN) {
            if (shift_held && key == scancode) keyboard_scrollback(key == SC_EXT_PGUP ? 1 : -1);
            return;
        }
    }

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
    case SC_ALT_PRESS:
        alt_held = 1;
        return;
    case SC_ALT_RELEASE:
        alt_held = 0;
        return;
    case SC_CAPS_PRESS:
        caps_lock = !caps_lock;
        return;
    }

    /* Ignore key releases (bit 7 set) */
    if (scancode & SC_RELEASE_BIT) return;

    key_count++;
    if (grabbed) return;

    /* Alt+F1-F6: VT switching */
    if (alt_held) {
        if (scancode >= SC_F1 && scancode <= SC_F6) {
            vt_switch(scancode - SC_F1);
            return;
        }
    }

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

uint32_t keyboard_key_count(void) {
    return key_count;
}

void keyboard_grab(int on) {
    grabbed = on;
}

void keyboard_init(void) {
    shift_held = 0;
    ctrl_held = 0;
    alt_held = 0;
    caps_lock = 0;
    extended = 0;

    /* Register IRQ 1 handler (vector 33) — follows PIT pattern */
    isr_register_handler(IRQ_BASE + 1, keyboard_irq_handler);

    /* Unmask IRQ 1 */
    pic_unmask(1);

    kprintf("[HAL] PS/2 keyboard initialized (IRQ 1, vector %d)\n", IRQ_BASE + 1);
}
