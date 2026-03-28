/* arc_os libc — printf family (adapted from kernel kprintf) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>

/* State for vsnprintf output target */
typedef struct {
    char  *buf;
    size_t pos;
    size_t size;
} PrintState;

static void ps_putchar(PrintState *ps, char c) {
    if (ps->buf && ps->pos + 1 < ps->size) {
        ps->buf[ps->pos] = c;
    }
    ps->pos++;
}

static void ps_print_uint64(PrintState *ps, uint64_t val, int base, int width, char pad) {
    char tmp[21];
    int i = 0;

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            uint64_t digit = val % base;
            tmp[i++] = (digit < 10) ? '0' + digit : 'a' + (digit - 10);
            val /= base;
        }
    }

    /* Pad to width */
    for (int j = i; j < width; j++) ps_putchar(ps, pad);

    /* Print digits in reverse */
    while (i > 0) ps_putchar(ps, tmp[--i]);
}

static void ps_print_int64(PrintState *ps, int64_t val, int width, char pad) {
    if (val < 0) {
        ps_putchar(ps, '-');
        if (width > 0) width--;
        if (val == INT64_MIN) {
            ps_print_uint64(ps, (uint64_t)INT64_MAX + 1, 10, width, pad);
            return;
        }
        val = -val;
    }
    ps_print_uint64(ps, (uint64_t)val, 10, width, pad);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    PrintState ps = { buf, 0, size };

    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            ps_putchar(&ps, fmt[i]);
            continue;
        }
        i++;
        if (!fmt[i]) break;

        /* Parse flags */
        char pad = ' ';
        if (fmt[i] == '0') { pad = '0'; i++; }

        /* Parse width */
        int width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }
        if (!fmt[i]) break;

        /* Length modifier */
        int is_long = 0;
        if (fmt[i] == 'l') { is_long = 1; i++; }
        if (!fmt[i]) break;

        switch (fmt[i]) {
        case 'd':
            if (is_long) ps_print_int64(&ps, va_arg(ap, int64_t), width, pad);
            else ps_print_int64(&ps, (int64_t)va_arg(ap, int32_t), width, pad);
            break;
        case 'u':
            if (is_long) ps_print_uint64(&ps, va_arg(ap, uint64_t), 10, width, pad);
            else ps_print_uint64(&ps, (uint64_t)va_arg(ap, uint32_t), 10, width, pad);
            break;
        case 'x':
            if (is_long) ps_print_uint64(&ps, va_arg(ap, uint64_t), 16, width, pad);
            else ps_print_uint64(&ps, (uint64_t)va_arg(ap, uint32_t), 16, width, pad);
            break;
        case 'p': {
            uint64_t val = (uint64_t)(uintptr_t)va_arg(ap, void *);
            ps_putchar(&ps, '0');
            ps_putchar(&ps, 'x');
            ps_print_uint64(&ps, val, 16, 16, '0');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) ps_putchar(&ps, *s++);
            break;
        }
        case 'c':
            ps_putchar(&ps, (char)va_arg(ap, int));
            break;
        case '%':
            ps_putchar(&ps, '%');
            break;
        default:
            ps_putchar(&ps, '%');
            if (is_long) ps_putchar(&ps, 'l');
            ps_putchar(&ps, fmt[i]);
            break;
        }
    }

    /* NUL-terminate */
    if (buf) {
        if (ps.pos < size) buf[ps.pos] = '\0';
        else if (size > 0) buf[size - 1] = '\0';
    }

    return (int)ps.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    if (len > 0) write(stream->fd, buf, (size_t)len);
    return len;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    if (len > 0) write(1, buf, (size_t)len);
    return len;
}
