/* arc_os — QR screens: page, panic and log codes on the framebuffer (M1-04c)
 *
 * Owns: the text that goes into the panic and log codes, the scale and
 * position of every code on screen, and the caption under a report or log
 * page. See qr_screen.h.
 * Invariants: a code is never drawn with fewer than QR_SCREEN_MIN_SCALE
 * pixels per module, and always with its full quiet zone on screen.
 * Locks: none of its own. Page drawing runs with the console held and paused
 * (diag/qr_mode.c); the panic functions run in panic mode and take nothing.
 * Cost: diagnostic and panic paths only. One encode is about 4 ms at
 * version 30 (M1-04b notes); a page writes every screen pixel once; the panic
 * encode retries at most nframes + 1 times when the backtrace must be cut. */

#include "diag/qr_screen.h"
#include "drivers/fb_console.h"
#include "lib/crc32.h"
#include "lib/klog.h"
#include "lib/ksyms.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "uapi/errno.h"
#include <stdarg.h>

uint32_t qr_screen_scale(uint32_t size, uint32_t avail_w, uint32_t avail_h) {
    uint32_t modules = size + 2 * QR_SCREEN_QUIET;
    uint32_t side = avail_w < avail_h ? avail_w : avail_h;
    uint32_t s = side / modules;
    return s >= QR_SCREEN_MIN_SCALE ? s : 0;
}

/* Draw qr with its quiet zone, top-left of the border at (x, y). */
static void draw_code(const QrCode *qr, uint32_t scale, uint32_t x, uint32_t y) {
    fb_console_draw_bitmap(qr->modules, (uint32_t)qr->size, (uint32_t)qr->size, QR_ROW_BYTES, QR_SCREEN_QUIET, scale, x,
                           y);
}

int qr_screen_show_page(const QrTextFormat *fmt, const char *text, const QrTextPage *pages, int npages, int index,
                        uint32_t seconds, QrCode *qr, char *payload) {
    int len = qr_text_page(fmt, text, pages, npages, index, payload, QR_BYTES_MAX);
    if (len < 0) return len;
    int ver = qr_encode((const uint8_t *)payload, (size_t)len, fmt->max_version, qr);
    if (ver < 0) return ver;

    uint32_t w = fb_console_width(), h = fb_console_height(), row_h = fb_console_row_height();
    uint32_t rows = fb_console_rows();
    if (rows < 2 || h <= row_h) return -ENOSPC;
    uint32_t avail_h = h - row_h;   /* the bottom text row holds the caption */
    uint32_t scale = qr_screen_scale((uint32_t)qr->size, w, avail_h);
    if (scale == 0) return -ENOSPC;
    uint32_t px = ((uint32_t)qr->size + 2 * QR_SCREEN_QUIET) * scale;

    char caption[128];
    uint32_t crc = crc32(0, text + pages[index].start, pages[index].len);
    int n = ksnprintf(caption, sizeof(caption), " arc_os %s  page %d/%d  crc %08x", fmt->kind, index + 1, npages, crc);
    if (seconds > 0 && n > 0 && (size_t)n < sizeof(caption))
        ksnprintf(caption + n, sizeof(caption) - (size_t)n, "  next in %u s, any key: next page", seconds);

    fb_console_fill(0xFFFFFF);
    draw_code(qr, scale, (w - px) / 2, (avail_h - px) / 2);
    fb_console_paint_row(rows - 1, caption, 0x000000, 0xFFFFFF);
    return 0;
}

/* Append to out[*pos..cap) like ksnprintf, keeping *pos on the NUL. */
static void append(char *out, size_t cap, size_t *pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void append(char *out, size_t cap, size_t *pos, const char *fmt, ...) {
    if (*pos + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(out + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *pos += (size_t)n < cap - *pos ? (size_t)n : cap - *pos - 1;
}

size_t qr_panic_text(const PanicInfo *info, uint32_t nframes, char *out, size_t cap) {
    size_t pos = 0;
    if (cap == 0) return 0;
    out[0] = '\0';
    if (nframes > info->nframes) nframes = info->nframes;
    append(out, cap, &pos, "PANIC %s\nCPU %u\n", info->reason != NULL ? info->reason : "?", info->cpu);
    for (uint32_t i = 0; i < info->nregs && i < PANIC_MAX_REGS; i++) {
        append(out, cap, &pos, "%s=%016lx%s", info->reg_names[i], info->reg_values[i],
               (i % 4 == 3 || i + 1 == info->nregs) ? "\n" : " ");
    }
    for (uint32_t i = 0; i < nframes; i++) {
        uint64_t off = 0;
        const char *name = ksym_lookup(info->frames[i], &off);
        if (name != NULL)
            append(out, cap, &pos, "#%u %016lx %s+0x%lx\n", i, info->frames[i], name, off);
        else
            append(out, cap, &pos, "#%u %016lx ?\n", i, info->frames[i]);
    }
    if (nframes < info->nframes) append(out, cap, &pos, "(%u more frames cut)\n", info->nframes - nframes);
    return pos;
}

int qr_panic_encode(const PanicInfo *info, const char *commit, QrPanicWork *w) {
    QrTextFormat fmt = {.commit = commit, .kind = "panic", .max_version = QR_VERSION_MAX};
    uint32_t nframes = info->nframes < PANIC_MAX_FRAMES ? info->nframes : PANIC_MAX_FRAMES;
    for (int64_t keep = nframes; keep >= 0; keep--) {
        size_t raw = qr_panic_text(info, (uint32_t)keep, w->raw, sizeof(w->raw));
        if (raw + 1 >= sizeof(w->raw) && keep > 0) continue;   /* cut short by the buffer: drop a frame */
        size_t len = qr_text_sanitize((const uint8_t *)w->raw, raw, w->text, sizeof(w->text));
        if (len > sizeof(w->text)) continue;
        QrTextPage page;
        if (qr_text_paginate(&fmt, w->text, len, &page, 1) != 1) continue;
        int plen = qr_text_page(&fmt, w->text, &page, 1, 0, w->payload, sizeof(w->payload));
        if (plen < 0) return plen;
        return qr_encode((const uint8_t *)w->payload, (size_t)plen, QR_VERSION_MAX, &w->qr);
    }
    return -E2BIG;
}

uint32_t qr_panic_scale(uint32_t row_h) {
    return row_h >= 32 ? 2 * QR_SCREEN_MIN_SCALE : QR_SCREEN_MIN_SCALE;
}

int qr_screen_draw_panic(const QrCode *qr) {
    uint32_t w = fb_console_width(), h = fb_console_height(), row_h = fb_console_row_height();
    if (h <= row_h) return -ENOSPC;
    uint32_t fit = qr_screen_scale((uint32_t)qr->size, w / 5 * 3, h - row_h);
    if (fit == 0) return -ENOSPC;
    /* As small as reads well (twice that where the console doubles its font),
     * so the registers beside it stay readable for a photo */
    uint32_t scale = qr_panic_scale(row_h);
    if (scale > fit) scale = fit;
    uint32_t px = ((uint32_t)qr->size + 2 * QR_SCREEN_QUIET) * scale;
    draw_code(qr, scale, w - px, row_h);
    return 0;
}

/* qr_log_text's copy state */
typedef struct {
    char *out;
    size_t cap, len;
    int full;
} LogCopy;

static void copy_line(const char *line, size_t len, void *ctx) {
    LogCopy *c = ctx;
    if (c->full || c->len + len + 1 > c->cap) {
        c->full = 1;
        return;
    }
    memcpy(c->out + c->len, line, len);
    c->out[c->len + len] = '\n';
    c->len += len + 1;
}

size_t qr_log_text(uint64_t window, int max_level, char *out, size_t cap) {
    uint64_t head = klog_head(), tail = klog_tail();
    uint64_t from = head - tail > window ? head - window : tail;
    LogCopy c = {.out = out, .cap = cap};
    klog_for_each_line(from, max_level, copy_line, &c);
    return c.len;
}
