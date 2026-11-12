#ifndef ARCHOS_DIAG_QR_TEXT_H
#define ARCHOS_DIAG_QR_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Plain-text pages for QR codes (M1-04b; drawn by M1-04c).
 *
 * A long text (boot report, log tail, panic) becomes numbered pages, each the
 * payload of one QR code:
 *
 *     ARC1 <commit> <kind> <page>/<pages> <crc32 of this page's text, 8 hex>
 *     <text lines>
 *     all <crc32 of all pages' text, 8 hex>      (last page only)
 *
 * The text is first passed through qr_text_sanitize, so a page holds only
 * printable ASCII and newlines and never a link. Concatenating the text of
 * every page in order gives back the sanitized text exactly. If the text does
 * not end in a newline, one is added before the "all" line; it belongs to no
 * page's text and no CRC covers it.
 *
 * Pure functions: no allocation, no globals, no locks, no floating point. */

/* Longest commit and kind strings a header accepts. */
#define QR_TEXT_COMMIT_MAX 40
#define QR_TEXT_KIND_MAX   16

/* What every page header says. commit and kind: 1..max printable characters,
 * no spaces (e.g. "a1b2c3d" or "unknown"; "report", "log", "panic"). */
typedef struct {
    const char *commit;
    const char *kind;
    int max_version;   /* largest QR version a page may need, 1..30 */
} QrTextFormat;

/* One page: text[start .. start + len) of the sanitized text. */
typedef struct {
    uint32_t start;
    uint32_t len;
} QrTextPage;

/* Copy in[0..len) to out, keeping printable ASCII (0x20-0x7E) and '\n'. Any
 * other byte becomes "\xNN" (lowercase hex) and every "://" becomes ":\/\/",
 * so the result can never hold a link. Writes at most cap bytes and never
 * cuts an escape in half (out is not NUL-terminated). Returns the length the
 * whole result needs; a return above cap means out holds a prefix of it. */
size_t qr_text_sanitize(const uint8_t *in, size_t len, char *out, size_t cap);

/* Split text[0..len) into pages that each fit one code of fmt->max_version
 * with its header (and the "all" line, which any page could carry). Pages
 * end at line ends; a line longer than a page is cut and continues on the
 * next page, never dropped. Empty text gives one empty page.
 * Returns the page count, -E2BIG if more than max_pages are needed, or
 * -EINVAL for a bad format (see QrTextFormat). Cost: at most two passes over
 * the text (forward, plus the search back to a line end on each page). */
int qr_text_paginate(const QrTextFormat *fmt, const char *text, size_t len, QrTextPage *pages, int max_pages);

/* Write the payload of page `index` (0-based) of npages from
 * qr_text_paginate into out; text is the same text (non-NULL, "" if empty).
 * Returns its length (at most qr_capacity(fmt->max_version), so a
 * QR_BYTES_MAX buffer always fits), -EINVAL for a bad argument, or -E2BIG if
 * cap is too small. The last page also reads every page's text for the "all"
 * CRC. */
int qr_text_page(const QrTextFormat *fmt, const char *text, const QrTextPage *pages, int npages, int index, char *out,
                 size_t cap);

#endif /* ARCHOS_DIAG_QR_TEXT_H */
