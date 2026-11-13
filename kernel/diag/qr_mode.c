/* arc_os — QR mode: the report or the log as QR pages, and the panic code (M1-04c)
 *
 * Owns: arc.qr=1 (the boot report, self-test and bench text as numbered ARC1
 * QR pages), arc.qr=log (the newest 8 KiB of the kernel log, snapshotted
 * once), the page loop (10 s or any key, forever; in test mode one pass), and
 * the panic screen hook that adds one code to the panic view.
 * Invariants: the pager draws only with the console held and paused, and
 * waits with it released, so kprintf from any CPU still reaches the serial
 * port; the panic hook allocates nothing and takes no lock (its buffers are
 * static and used by nothing else).
 * Locks: the console owner (klog_console_acquire, a try-only flag, not a
 * spinlock) around each page draw; none on the panic path.
 * Cost: diagnostic mode and panics only. A page costs one encode and one
 * full-screen write; the texts are built once (report: O(lines); log:
 * O(8 KiB)). */

#include "diag/diag.h"
#include "diag/qr_screen.h"
#include "drivers/fb_console.h"
#include "lib/build_commit.h"
#include "lib/cmdline.h"
#include "lib/klog.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/testmode.h"
#include "mm/kmalloc.h"

#define QR_PAGE_SECONDS  10      /* as the text pager */
#define QR_TEST_WAIT_S   60      /* test mode: the harness presses a key per page */
#define QR_LOG_BYTES     8192    /* arc.qr=log: the newest 8 KiB of the log */
#define QR_MAX_PAGES     64      /* report: 320 lines of 120 bytes is about 30 pages */
#define QR_PANIC_HOLD_MS 60000   /* arc.qrhold: how long the panic code stays up in test mode */

static QrPanicWork panic_work;   /* static: the panic path never allocates */
static int panic_hold;

/* Test mode: end the run with the page count as passes. */
static void qr_test_end(int passes, int fails) __attribute__((noreturn));
static void qr_test_end(int passes, int fails) {
    kprintf("ARC_TEST DONE pass=%d fail=%d\n", passes, fails);
    testmode_exit(fails == 0 ? 0 : 1);
}

/* The kept report lines, one '\n' after each, in a new buffer. */
static char *report_text(size_t *len) {
    size_t n = 0;
    for (uint32_t i = 0; i < diag_line_count(); i++) n += strlen(diag_line_text(i)) + 1;
    char *buf = kmalloc(n + 1, GFP_KERNEL);
    if (buf == NULL) return NULL;
    size_t pos = 0;
    for (uint32_t i = 0; i < diag_line_count(); i++) {
        const char *s = diag_line_text(i);
        size_t l = strlen(s);
        memcpy(buf + pos, s, l);
        pos += l;
        buf[pos++] = '\n';
    }
    *len = pos;
    return buf;
}

/* Draw page `index` with the console held and paused. Returns qr_screen_show_page's result. */
static int show_page(const QrTextFormat *fmt, const char *text, const QrTextPage *pages, int npages, int index,
                     QrCode *qr, char *payload) {
    int owned = klog_console_acquire();
    fb_console_pause();
    int rc = qr_screen_show_page(fmt, text, pages, npages, index, testmode_active() ? 0 : QR_PAGE_SECONDS, qr, payload);
    if (rc != 0) fb_console_resume();   /* nothing drawn: give the text screen back */
    if (owned) klog_console_release();
    return rc;
}

void diag_qr_run(int can_halt, int passes, int fails) {
    char val[8] = "";
    cmdline_get(cmdline_raw(), "arc.qr", val, sizeof(val));
    int log_mode = strcmp(val, "log") == 0;
    QrTextFormat fmt = {.commit = arc_build_commit, .kind = log_mode ? "log" : "report", .max_version = 0};
    /* The largest version that fits this screen at the minimum scale, so every page fits */
    uint32_t page_h = qr_screen_page_height();
    for (int v = QR_VERSION_MAX; v >= 1 && fmt.max_version == 0 && page_h > 0; v--) {
        if (qr_screen_scale((uint32_t)(17 + 4 * v), fb_console_width(), page_h) != 0) fmt.max_version = v;
    }

    size_t raw_len = 0;
    char *raw;
    if (log_mode) {
        raw = kmalloc(QR_LOG_BYTES, GFP_KERNEL);
        if (raw != NULL) raw_len = qr_log_text(QR_LOG_BYTES, klog_console_level(), raw, QR_LOG_BYTES);
    } else {
        raw = report_text(&raw_len);
    }
    /* Sanitizing can grow the text 4x (every byte an escape) */
    char *text = raw != NULL ? kmalloc(4 * raw_len + 1, GFP_KERNEL) : NULL;
    QrTextPage *pages = kmalloc(sizeof(QrTextPage) * QR_MAX_PAGES, GFP_KERNEL);
    QrCode *qr = kmalloc(sizeof(QrCode), GFP_KERNEL);
    char *payload = kmalloc(QR_BYTES_MAX, GFP_KERNEL);
    int npages = -1, rc = -1;
    if (text != NULL && pages != NULL && qr != NULL && payload != NULL && fmt.max_version > 0) {
        size_t len = qr_text_sanitize((const uint8_t *)raw, raw_len, text, 4 * raw_len + 1);
        npages = qr_text_paginate(&fmt, text, len, pages, QR_MAX_PAGES);
        if (npages > 0) rc = show_page(&fmt, text, pages, npages, 0, qr, payload);
    }
    if (rc != 0) {
        kprintf("[QR] can't show %s codes (screen %ux%u, pages %d, error %d): text pages instead\n", fmt.kind,
                fb_console_width(), fb_console_height(), npages, rc);
        if (testmode_active()) qr_test_end(passes, fails + 1);
        kfree(raw);
        kfree(text);
        kfree(pages);
        kfree(qr);
        kfree(payload);
        return;
    }

    int shown = 0, timeouts = 0;
    for (int page = 0;;) {
        if (testmode_active()) {
            /* The qr suite takes a screendump on this line, then presses a key */
            kprintf("ARC_QR shown %s %d/%d\n", fmt.kind, page + 1, npages);
            if (diag_wait_key(QR_TEST_WAIT_S, can_halt)) {
                kprintf("ARC_TEST PASS qr-%s-page-%d\n", fmt.kind, page + 1);
                shown++;
            } else {
                kprintf("ARC_TEST FAIL qr-%s-page-%d no key within %d s\n", fmt.kind, page + 1, QR_TEST_WAIT_S);
                timeouts++;
            }
            if (page + 1 == npages) qr_test_end(passes + shown, fails + timeouts);
        } else {
            diag_wait_key(QR_PAGE_SECONDS, can_halt);
        }
        page = (page + 1) % npages;
        if (show_page(&fmt, text, pages, npages, page, qr, payload) != 0 && testmode_active()) {
            kprintf("ARC_TEST FAIL qr-%s-page-%d not drawn\n", fmt.kind, page + 1);
            timeouts++;
        }
    }
}

void diag_qr_panic_setup(void) {
    panic_hold = testmode_active() && diag_flag_on("arc.qrhold");
}

void diag_qr_panic_screen(const PanicInfo *info) {
    fb_console_panic_view(0);   /* the text first, in case the encode goes wrong */
    if (fb_console_width() == 0) return;
    int ver = qr_panic_encode(info, arc_build_commit, &panic_work);
    int rc = ver > 0 ? qr_screen_draw_panic(&panic_work.qr) : ver;
    if (rc != 0) {
        kprintf_level(KLOG_EMERG, "[PANIC] no QR code on screen (error %d)\n", rc);
        return;
    }
    if (panic_hold) {
        /* The qr suite takes a screendump on this line and then ends QEMU */
        kprintf_level(KLOG_EMERG, "ARC_QR shown panic 1/1\n");
        diag_delay_ms(QR_PANIC_HOLD_MS);
    }
}
