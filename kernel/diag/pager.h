#ifndef ARCHOS_DIAG_PAGER_H
#define ARCHOS_DIAG_PAGER_H

#include <stddef.h>
#include <stdint.h>

/* How the diagnostic pagers (text report and QR pages) hold a page, so a
 * person can photograph or copy it.
 *
 * A page stays on screen until a key is pressed. Until the first key of the
 * pager run, a page also turns by itself after PAGER_AUTO_SECONDS with a
 * countdown on screen, so a machine whose keyboard doesn't work still shows
 * every page. The first key proves the keyboard works: from then on
 * ("manual"), a page stays until the next key, however long that takes.
 *
 * This part is the arithmetic and the on-screen hint, host-tested; the wait
 * itself is diag_page_hold (diag/diag.h). */

#define PAGER_AUTO_SECONDS 60   /* no key yet: a page turns by itself after this */

/* Whole seconds left until end_ns at now_ns, rounded up (a countdown shows
 * "1 s" until the deadline, never "0 s" before it); 0 once now_ns >= end_ns. */
uint32_t pager_seconds_left(uint64_t now_ns, uint64_t end_ns);

/* Write the key hint that follows "page n/N" on a pager's banner or caption:
 * "any key: next page (auto in <seconds_left> s)" while no key has been
 * pressed, "any key: next page" once manual. Same contract as ksnprintf:
 * NUL-terminated, cut if cap is too small; returns the full length. */
int pager_hint(char *buf, size_t cap, int manual, uint32_t seconds_left);

#endif /* ARCHOS_DIAG_PAGER_H */
