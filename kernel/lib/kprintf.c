#include "lib/kprintf.h"
#include "arch/x86_64/serial.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Secondary output hook (e.g., framebuffer console) */
static void (*putchar_hook)(char c);

void kprintf_set_putchar_hook(void (*hook)(char c)) {
    putchar_hook = hook;
}

/* Output one character to serial + optional hook. */
static void emit(char c) {
    serial_putchar(c);
    if (putchar_hook) putchar_hook(c);
}

static void print_uint64(uint64_t val, int base) {
    char buf[21]; /* max 20 digits for uint64 in decimal + NUL */
    int i = 0;

    if (val == 0) {
        emit('0');
        return;
    }

    while (val > 0) {
        uint64_t digit = val % base;
        buf[i++] = (digit < 10) ? '0' + digit : 'a' + (digit - 10);
        val /= base;
    }

    /* Print digits in reverse order. */
    while (i > 0) {
        emit(buf[--i]);
    }
}

static void print_int64(int64_t val) {
    if (val < 0) {
        emit('-');
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
    emit('0');
    emit('x');

    /* Print exactly 16 hex digits, zero-padded. */
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (val >> shift) & 0xF;
        emit((nibble < 10) ? '0' + nibble : 'a' + (nibble - 10));
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (int i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            emit(fmt[i]);
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
                emit(*s++);
            }
            break;
        }
        case '%':
            emit('%');
            break;
        default:
            /* Unknown specifier — print literally. */
            emit('%');
            if (is_long) {
                emit('l');
            }
            emit(fmt[i]);
            break;
        }
    }

    va_end(ap);
}
