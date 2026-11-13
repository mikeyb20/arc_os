/* qr_text.c — text pages for QR codes: sanitize, split, header and CRCs (M1-04b).
 *
 * Owns the ARC1 page format described in qr_text.h. M1-04c feeds it the boot
 * report, the log tail and the panic text and draws each page with qr_encode.
 * Pure functions on caller buffers: no allocation, no globals, no locks. */

#include "diag/qr_text.h"
#include "diag/qr.h"
#include "lib/crc32.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "uapi/errno.h"

/* "\nall xxxxxxxx\n": the "all" line with the newline that may precede it */
#define ALL_LINE_MAX 14

/* Append piece[0..n) if it fits whole; once one piece does not fit, nothing
 * after it is written either, so out always holds a prefix of the result. */
static void sanitize_emit(char *out, size_t cap, size_t *pos, int *full, const char *piece, size_t n) {
    if (!*full && *pos + n <= cap)
        memcpy(out + *pos, piece, n);
    else
        *full = 1;
    *pos += n;
}

size_t qr_text_sanitize(const uint8_t *in, size_t len, char *out, size_t cap) {
    size_t pos = 0;
    int full = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == ':' && i + 2 < len && in[i + 1] == '/' && in[i + 2] == '/') {
            sanitize_emit(out, cap, &pos, &full, ":\\/\\/", 5);
            i += 2;
        } else if ((c >= 0x20 && c <= 0x7E) || c == '\n') {
            char ch = (char)c;
            sanitize_emit(out, cap, &pos, &full, &ch, 1);
        } else {
            char esc[5];
            ksnprintf(esc, sizeof(esc), "\\x%02x", c);
            sanitize_emit(out, cap, &pos, &full, esc, 4);
        }
    }
    return pos;
}

/* 1..max printable characters with no spaces */
static int header_word_ok(const char *s, size_t max) {
    if (s == NULL || s[0] == '\0') return 0;
    size_t i = 0;
    for (; s[i] != '\0'; i++) {
        if (i >= max || s[i] <= 0x20 || s[i] > 0x7E) return 0;
    }
    return 1;
}

static int format_ok(const QrTextFormat *fmt) {
    return fmt != NULL && fmt->max_version >= 1 && fmt->max_version <= QR_VERSION_MAX &&
           header_word_ok(fmt->commit, QR_TEXT_COMMIT_MAX) && header_word_ok(fmt->kind, QR_TEXT_KIND_MAX);
}

static size_t decimal_digits(uint32_t v) {
    size_t d = 1;
    while (v >= 10) {
        v /= 10;
        d++;
    }
    return d;
}

/* Text bytes one page may hold: the code's capacity minus the longest header
 * any page of at most max_pages can have and the "all" line. */
static size_t body_max(const QrTextFormat *fmt, int max_pages) {
    size_t digits = decimal_digits((uint32_t)max_pages);
    /* "ARC1 " commit " " kind " " page "/" pages " " 8 hex "\n" */
    size_t header = 5 + strlen(fmt->commit) + 1 + strlen(fmt->kind) + 1 + 2 * digits + 1 + 1 + 8 + 1;
    size_t cap = qr_capacity(fmt->max_version);
    return cap > header + ALL_LINE_MAX ? cap - header - ALL_LINE_MAX : 0;
}

int qr_text_paginate(const QrTextFormat *fmt, const char *text, size_t len, QrTextPage *pages, int max_pages) {
    if (!format_ok(fmt) || pages == NULL || max_pages < 1 || (text == NULL && len > 0) || len > UINT32_MAX) {
        return -EINVAL;
    }
    size_t room = body_max(fmt, max_pages);
    if (room == 0) return -EINVAL;

    int n = 0;
    size_t pos = 0;
    do {
        if (n == max_pages) return -E2BIG;
        size_t take = len - pos;
        if (take > room) {
            /* End at the last line end that fits; a longer line is cut */
            take = room;
            while (take > 0 && text[pos + take - 1] != '\n') take--;
            if (take == 0) take = room;
        }
        pages[n].start = (uint32_t)pos;
        pages[n].len = (uint32_t)take;
        n++;
        pos += take;
    } while (pos < len);
    return n;
}

int qr_text_page(const QrTextFormat *fmt, const char *text, const QrTextPage *pages, int npages, int index, char *out,
                 size_t cap) {
    if (!format_ok(fmt) || text == NULL || pages == NULL || out == NULL || npages < 1 || index < 0 || index >= npages) {
        return -EINVAL;
    }
    const char *body = text + pages[index].start;
    size_t blen = pages[index].len;
    char line[QR_TEXT_COMMIT_MAX + QR_TEXT_KIND_MAX + 48];
    int hlen = ksnprintf(line, sizeof(line), "ARC1 %s %s %d/%d %08x\n", fmt->commit, fmt->kind, index + 1, npages,
                         crc32(0, body, blen));

    size_t total = (size_t)hlen + blen;
    char all[ALL_LINE_MAX + 1];
    int alen = 0;
    if (index == npages - 1) {
        uint32_t crc = 0;
        for (int i = 0; i < npages; i++) crc = crc32(crc, text + pages[i].start, pages[i].len);
        int nl = blen > 0 && body[blen - 1] != '\n';
        alen = ksnprintf(all, sizeof(all), "%sall %08x\n", nl ? "\n" : "", crc);
        total += (size_t)alen;
    }
    if (total > cap) return -E2BIG;

    memcpy(out, line, (size_t)hlen);
    memcpy(out + hlen, body, blen);
    memcpy(out + hlen + blen, all, (size_t)alen);
    return (int)total;
}
