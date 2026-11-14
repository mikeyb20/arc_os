/* arc_os — a static program over 9 MiB (M1-12). Test ISOs only.
 *
 * exec maps the whole pages of its segments from the page cache instead of
 * copying the file (and there is no size limit). The payload
 * (bigexec_data.S) is 8 MiB of initialized read-only data and 1 MiB of
 * initialized writable data, plus 1 MiB of bss here. The program checks
 * every word, writes to the writable data (copy on write: the file and the
 * next run never see it) and to the bss, and checks again. Suite
 * "pagecache" runs it twice. Exit status: 0, or which part was wrong. */

#include <stdint.h>
#include <stdio.h>

#define RO_PAGES 2048
#define RW_PAGES 256

extern const uint32_t big_ro[RO_PAGES * 1024];
extern uint32_t big_rw[RW_PAGES * 1024];
static uint32_t big_bss[256 * 1024];

static uint32_t word(uint32_t k, uint32_t w, uint32_t mask) {
    uint32_t j = w / 16;
    if (w % 16 == 0) return ((k << 8) | j) ^ mask;
    return (uint32_t)((uint64_t)(k * 64 + j) * 0x9E3779B1u) ^ mask;
}

/* Pages of a[] that hold something other than the pattern (skip: words
 * this run changed) */
static unsigned bad_pages(const uint32_t *a, unsigned pages, uint32_t mask, int skip_first) {
    unsigned bad = 0;
    for (uint32_t k = 0; k < pages; k++) {
        for (uint32_t w = skip_first ? 1 : 0; w < 1024; w++) {
            if (a[k * 1024 + w] != word(k, w, mask)) {
                bad++;
                break;
            }
        }
    }
    return bad;
}

int main(void) {
    int status = 0;
    unsigned ro = bad_pages(big_ro, RO_PAGES, 0, 0);
    unsigned rw = bad_pages(big_rw, RW_PAGES, 0xFFFFFFFF, 0);
    if (ro != 0) status |= 1;
    if (rw != 0) status |= 2;   /* also on a second run if a store reached the file */
    for (uint32_t k = 0; k < sizeof(big_bss) / 4; k++) {
        if (big_bss[k] != 0) {
            status |= 4;
            break;
        }
    }
    /* every writable page gets its own copy; the rest of each page stays */
    for (uint32_t k = 0; k < RW_PAGES; k++) big_rw[k * 1024] = 0xC0FFEE00u + k;
    for (uint32_t k = 0; k < sizeof(big_bss) / 4; k += 1024) big_bss[k] = k + 1;
    if (bad_pages(big_rw, RW_PAGES, 0xFFFFFFFF, 1) != 0) status |= 8;
    for (uint32_t k = 0; k < RW_PAGES; k++) {
        if (big_rw[k * 1024] != 0xC0FFEE00u + k) status |= 16;
    }
    for (uint32_t k = 0; k < sizeof(big_bss) / 4; k += 1024) {
        if (big_bss[k] != k + 1) status |= 32;
    }
    printf("bigexec: %u bad read-only pages, %u bad writable pages, status %d\n", ro, rw, status);
    return status;
}
