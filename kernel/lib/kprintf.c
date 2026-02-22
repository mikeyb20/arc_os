#include "lib/kprintf.h"
#include "arch/x86_64/serial.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void print_uint64(uint64_t val, int base) {
    char buf[21]; /* max 20 digits for uint64 in decimal + NUL */
    int i = 0;

    if (val == 0) {
        serial_putchar('0');
        return;
    }

    while (val > 0) {
        uint64_t digit = val % base;
        buf[i++] = (digit < 10) ? '0' + digit : 'a' + (digit - 10);
        val /= base;
    }

    /* Print digits in reverse order. */
    while (i > 0) {
        serial_putchar(buf[--i]);
    }
}

static void print_int64(int64_t val) {
    if (val < 0) {
        serial_putchar('-');
        /* Handle INT64_MIN: -(INT64_MIN) overflows, so cast after negate. */
        if (val == INT64_MIN) {
            print_uint64((uint64_t)INT64_MAX + 1, 10);
            return;
        }
        val = -val;
    }
    print_uint64((uint64_t)val, 10);
}

static void print_ptr(uint64_t val) {
    serial_putchar('0');
    serial_putchar('x');

    /* Print exactly 16 hex digits, zero-padded. */
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t nibble = (val >> i) & 0xF;
        serial_putchar((nibble < 10) ? '0' + nibble : 'a' + (nibble - 10));
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (int i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            serial_putchar(fmt[i]);
            continue;
        }

        i++; /* skip '%' */
        if (fmt[i] == '\0') {
            break;
        }

        /* Check for 'l' length modifier. */
        bool is_long = false;
        if (fmt[i] == 'l') {
            is_long = true;
            i++;
            if (fmt[i] == '\0') {
                break;
            }
        }

        switch (fmt[i]) {
        case 'd':
            if (is_long) {
                print_int64(va_arg(ap, int64_t));
            } else {
                print_int64((int64_t)va_arg(ap, int32_t));
            }
            break;
        case 'u':
            if (is_long) {
                print_uint64(va_arg(ap, uint64_t), 10);
            } else {
                print_uint64((uint64_t)va_arg(ap, uint32_t), 10);
            }
            break;
        case 'x':
            if (is_long) {
                print_uint64(va_arg(ap, uint64_t), 16);
            } else {
                print_uint64((uint64_t)va_arg(ap, uint32_t), 16);
            }
            break;
        case 'p':
            print_ptr((uint64_t)(uintptr_t)va_arg(ap, void *));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            while (*s) {
                serial_putchar(*s++);
            }
            break;
        }
        case '%':
            serial_putchar('%');
            break;
        default:
            /* Unknown specifier — print literally. */
            serial_putchar('%');
            if (is_long) {
                serial_putchar('l');
            }
            serial_putchar(fmt[i]);
            break;
        }
    }

    va_end(ap);
}
