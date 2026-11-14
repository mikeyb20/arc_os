/* arc_os — Pager hold: countdown arithmetic and key hint (bring-up pages)
 *
 * Owns: the text that tells the person how a report or QR page turns, and
 * the seconds shown in its countdown. See pager.h.
 * Invariants: the countdown never shows 0 before its deadline.
 * Locks: none (pure functions).
 * Cost: diagnostic mode only; O(1) per call, about once a second. */

#include "diag/pager.h"
#include "lib/kprintf.h"
#include "time/clock.h"

uint32_t pager_seconds_left(uint64_t now_ns, uint64_t end_ns) {
    if (now_ns >= end_ns) return 0;
    uint64_t d = end_ns - now_ns;
    uint64_t s = d / NSEC_PER_SEC + (d % NSEC_PER_SEC != 0);   /* rounded up without overflow */
    return s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
}

int pager_hint(char *buf, size_t cap, int manual, uint32_t seconds_left) {
    if (manual) return ksnprintf(buf, cap, "any key: next page");
    return ksnprintf(buf, cap, "any key: next page (auto in %u s)", seconds_left);
}
