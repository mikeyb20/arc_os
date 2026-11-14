#ifndef ARCHOS_DIAG_QR_SCREEN_H
#define ARCHOS_DIAG_QR_SCREEN_H

#include "diag/qr.h"
#include "diag/qr_text.h"
#include "lib/panic.h"
#include <stddef.h>
#include <stdint.h>

/* QR screens (M1-04c): what the boot-report pager, the log pager and the
 * panic screen put in their codes, and how a code lands on the framebuffer.
 * The run loops live in diag/qr_mode.c; this part is host-tested.
 *
 * A code is drawn black on white through fb_console_draw_bitmap, with a
 * QR_SCREEN_QUIET-module white border, whole pixels per module, never fewer
 * than QR_SCREEN_MIN_SCALE (phones read 4+ pixel modules off a screen
 * reliably). Callers of the drawing functions hold the console
 * (klog_console_acquire) and have paused it, except on the panic path. */

#define QR_SCREEN_QUIET     4   /* white border, in modules (the standard's minimum) */
#define QR_SCREEN_MIN_SCALE 4   /* pixels per module */

/* Largest panic text kept (a code holds at most QR_BYTES_MAX bytes anyway) */
#define QR_PANIC_TEXT_MAX 1536

/* Everything one panic code needs, kept static by the caller: the panic path
 * never allocates. About 11 KiB. */
typedef struct {
    char raw[QR_PANIC_TEXT_MAX];    /* text as built from the PanicInfo */
    char text[QR_PANIC_TEXT_MAX];   /* the same after qr_text_sanitize */
    char payload[QR_BYTES_MAX];     /* the one ARC1 page */
    QrCode qr;
} QrPanicWork;

/* Pixels per module for a code of `size` modules plus its quiet zone in a box
 * of avail_w x avail_h pixels: the largest whole number that fits, or 0 if
 * that is under QR_SCREEN_MIN_SCALE. */
uint32_t qr_screen_scale(uint32_t size, uint32_t avail_w, uint32_t avail_h);

/* Height in pixels a report or log page's code may use: every text row but
 * the bottom one, which holds the caption (0 before init or under 2 rows). */
uint32_t qr_screen_page_height(void);

/* Encode page `index` of npages (qr_text_paginate over text) into qr, using
 * payload (QR_BYTES_MAX bytes) as scratch, and show it: the screen white, the
 * code centred above the bottom text row, which says "arc_os <kind>  page n/N
 * crc <crc of the page's text>" and, when seconds > 0, when the next page
 * comes. Returns 0, -ENOSPC if the screen is too small for the code at
 * QR_SCREEN_MIN_SCALE (nothing drawn), or a qr_text_page / qr_encode error.
 * Cost: one encode (about 4 ms at version 30) and one full-screen write. */
int qr_screen_show_page(const QrTextFormat *fmt, const char *text, const QrTextPage *pages, int npages, int index,
                        uint32_t seconds, QrCode *qr, char *payload);

/* Write the panic text for the first nframes frames of info into out (cap
 * bytes, NUL-terminated, cut if too small): "PANIC <reason>", "CPU <n>", the
 * registers four to a line as NAME=value, then one "#i address symbol+off"
 * line per frame, and "(<k> more frames cut)" when nframes < info->nframes.
 * Returns the length. Allocates nothing; safe on the panic path. */
size_t qr_panic_text(const PanicInfo *info, uint32_t nframes, char *out, size_t cap);

/* Build the panic code for info into w->qr: one page of kind "panic" at
 * version 30 at most, with as many backtrace frames as fit (cut from the
 * bottom, and the text says so). Returns the QR version, or -E2BIG if not even
 * the frameless text fits. Allocates nothing, takes no lock. */
int qr_panic_encode(const PanicInfo *info, const char *commit, QrPanicWork *w);

/* Pixels per module the panic code wants: QR_SCREEN_MIN_SCALE, doubled when
 * text rows are 32 pixels high (the console's 2x font on large screens). */
uint32_t qr_panic_scale(uint32_t row_h);

/* Draw an encoded panic code at the top right of the screen, under the
 * banner row, at qr_panic_scale (less only if the code would then take more
 * than 3/5 of the width or the height under the banner), after redrawing the
 * panic view wrapped to the columns left of it (fb_console_panic_view), so
 * the registers and backtrace stay readable beside it for a photo. Returns 0,
 * or -ENOSPC if it doesn't fit at QR_SCREEN_MIN_SCALE (nothing redrawn).
 * Pixels only; takes no lock (panic path). */
int qr_screen_draw_panic(const QrCode *qr);

/* Copy the printable log lines (level at or below max_level, markers
 * stripped, one '\n' after each) from the newest `window` bytes of the log
 * ring into out, at most cap bytes; a line that doesn't fit whole ends the
 * copy. Returns the length. Cost: O(window). */
size_t qr_log_text(uint64_t window, int max_level, char *out, size_t cap);

#endif /* ARCHOS_DIAG_QR_SCREEN_H */
