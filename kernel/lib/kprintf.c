/* arc_os — kprintf, kprintf_level and ksnprintf: one formatter, two outputs
 *
 * Owns: printf-style formatting for the kernel. kprintf formats into a small
 * stack buffer and hands each full buffer (and the rest at the end) to the
 * log ring (lib/klog.c); ksnprintf formats into the caller's buffer.
 * Invariants: no allocation, no locks here (klog_write takes the log lock);
 * the stack cost is one 128-byte buffer, so it is safe on 4 KiB IST stacks.
 * Locks: none. */

#include "lib/kprintf.h"
#include "lib/klog.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where formatted characters go */
typedef struct {
    char *buf;
    size_t size;      /* capacity of buf */
    size_t len;       /* characters stored in buf */
    size_t total;     /* characters produced (ksnprintf's return value) */
    int level;        /* klog level; -1 for ksnprintf */
} FmtOut;

static void out_char(FmtOut *o, char c) {
    o->total++;
    if (o->level >= 0) {
        if (o->len == o->size) {
            klog_write(o->level, o->buf, o->len);
            o->len = 0;
        }
        o->buf[o->len++] = c;
    } else if (o->len + 1 < o->size) {
        o->buf[o->len++] = c;
    }
}

static void out_pad(FmtOut *o, char c, int n) {
    while (n-- > 0) out_char(o, c);
}

/* Write the digits of val in base (10 or 16) with padding. */
static void out_number(FmtOut *o, uint64_t val, int base, bool upper, bool negative, int width, bool left, bool zero) {
    char digits[20];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = set[val % (uint64_t)base];
        val /= (uint64_t)base;
    } while (val != 0);

    int len = n + (negative ? 1 : 0);
    if (!left && !zero) out_pad(o, ' ', width - len);
    if (negative) out_char(o, '-');
    if (!left && zero) out_pad(o, '0', width - len);
    while (n > 0) out_char(o, digits[--n]);
    if (left) out_pad(o, ' ', width - len);
}

static void format(FmtOut *o, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            out_char(o, *p);
            continue;
        }
        const char *spec = p++;   /* the '%', for unknown conversions */
        bool left = false, zero = false;
        for (;; p++) {
            if (*p == '-')
                left = true;
            else if (*p == '0')
                zero = true;
            else
                break;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') precision = precision * 10 + (*p++ - '0');
        }
        bool is_long = false;
        while (*p == 'l') {
            is_long = true;
            p++;
        }
        if (*p == '\0') break;

        switch (*p) {
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int32_t);
            /* INT64_MIN: negate in unsigned arithmetic */
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            out_number(o, mag, 10, false, v < 0, width, left, zero);
            break;
        }
        case 'u':
            out_number(o, is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 10, false, false, width, left, zero);
            break;
        case 'x':
        case 'X':
            out_number(o, is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 16, *p == 'X', false, width, left,
                       zero);
            break;
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            out_char(o, '0');
            out_char(o, 'x');
            out_number(o, v, 16, false, false, 16, false, true);
            break;
        }
        case 'c':
            out_pad(o, ' ', left ? 0 : width - 1);
            out_char(o, (char)va_arg(ap, int));
            out_pad(o, ' ', left ? width - 1 : 0);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) s = "(null)";
            int len = 0;
            while (s[len] != '\0' && (precision < 0 || len < precision)) len++;
            if (!left) out_pad(o, ' ', width - len);
            for (int i = 0; i < len; i++) out_char(o, s[i]);
            if (left) out_pad(o, ' ', width - len);
            break;
        }
        case '%':
            out_char(o, '%');
            break;
        default:
            /* Unknown conversion: print it as written */
            for (const char *q = spec; q <= p; q++) out_char(o, *q);
            break;
        }
    }
}

static void vlog(int level, const char *fmt, va_list ap) {
    char buf[128];
    FmtOut o = {.buf = buf, .size = sizeof(buf), .len = 0, .total = 0, .level = level};
    format(&o, fmt, ap);
    if (o.len > 0) klog_write(level, buf, o.len);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(KLOG_INFO, fmt, ap);
    va_end(ap);
}

void kprintf_level(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(level, fmt, ap);
    va_end(ap);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    FmtOut o = {.buf = buf, .size = size, .len = 0, .total = 0, .level = -1};
    format(&o, fmt, ap);
    if (size > 0) buf[o.len] = '\0';
    return (int)o.total;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
