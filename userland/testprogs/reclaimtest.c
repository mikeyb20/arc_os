/* arc_os — page reclaim under memory pressure (M1-14), under musl. Test
 * ISOs only. Runs in QEMU (suite "reclaim", -m 512M) with the disk from
 * `tools/make-arc-test-disks.sh make-big`: /system/big.bin is a 2 GiB sparse
 * ext2 file, four times the machine's memory. In each 8 MiB stretch b its
 * first 64 KiB are data (64-bit word j = b << 32 | j), the rest a hole.
 *   idle   nothing dirty at boot: the flusher's timer is not armed;
 *   read   read() through the whole file, every word checked: the page cache
 *          only holds it because reclaim frees the pages behind the reader,
 *          and free memory never went below the min watermark;
 *   mmap   the same through one shared mapping: reclaim unmaps the pages it
 *          frees (each mapping's entry, with a TLB shootdown) and a later
 *          access faults them back in (direct reclaim may help kswapd here);
 *          a private copy of the file's first page keeps its written word;
 *   meminfo  /proc/meminfo shows Cached, Dirty, Writeback, Active, Inactive.
 * Prints the reclaim counters, one FAIL line per failed check; exit status =
 * number of failures. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL reclaimtest:%d: %s (errno %d)\n", __LINE__, #cond, errno); \
            failures++;                                                             \
        }                                                                           \
    } while (0)

#define BIG     "/system/big.bin"
#define SIZE    (2ull << 30)
#define STRETCH (8ull << 20)
#define DATA    (64ull << 10)
#define CHUNK   (1u << 20)

static char text[16384];

/* One counter from /proc/vmstat ("name value" lines); -1 if missing */
static long long vm_counter(const char *name) {
    int fd = open("/proc/vmstat", O_RDONLY);
    if (fd < 0) return -1;
    text[0] = '\n';   /* every line, the first too, starts after a newline */
    ssize_t n = read(fd, text + 1, sizeof(text) - 2);
    close(fd);
    if (n <= 0) return -1;
    text[n + 1] = 0;
    char key[64];
    snprintf(key, sizeof(key), "\n%s ", name);
    char *p = strstr(text, key);
    return p != NULL ? atoll(p + strlen(key)) : -1;
}

static void counters(const char *when) {
    static const char *names[] = {"nr_free_pages", "nr_file_pages",  "nr_active_file",   "nr_inactive_file",
                                  "pgscan_kswapd", "pgsteal_kswapd", "pgscan_direct",    "pgsteal_direct",
                                  "allocstall",    "kswapd_wakeups", "reclaim_unmapped", "free_lowest",
                                  "watermark_min", "watermark_low",  "watermark_high"};
    printf("reclaimtest: %s:", when);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) printf(" %s %lld", names[i], vm_counter(names[i]));
    printf("\n");
}

/* Bad 4 KiB pages in a piece of the file at byte offset off */
static uint64_t check_piece(const uint64_t *w, uint64_t off, uint64_t len) {
    uint64_t bad = 0;
    for (uint64_t p = 0; p < len; p += 4096) {
        uint64_t at = off + p;
        uint64_t b = at / STRETCH, in = at % STRETCH;
        const uint64_t *pw = w + p / 8;
        int ok = 1;
        if (in < DATA) {
            uint64_t first = (b << 32) | (in / 8);
            for (int k = 0; k < 512; k++) ok &= pw[k] == first + (uint64_t)k;
        } else {
            uint64_t any = 0;
            for (int k = 0; k < 512; k++) any |= pw[k];
            ok = any == 0;
        }
        bad += !ok;
    }
    return bad;
}

static void test_idle(void) {
    CHECK(vm_counter("wb_timer_armed") == 0);
    CHECK(vm_counter("wb_listed") == 0);
    printf("reclaimtest: idle ok\n");
}

static void test_read(void) {
    int f0 = failures;
    long long steal0 = vm_counter("pgsteal_kswapd") + vm_counter("pgsteal_direct");
    int fd = open(BIG, O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    uint64_t *buf = malloc(CHUNK);
    CHECK(buf != NULL);
    uint64_t total = 0, bad = 0;
    for (;;) {
        ssize_t n = read(fd, buf, CHUNK);
        CHECK(n >= 0);
        if (n <= 0) break;
        bad += check_piece(buf, total, (uint64_t)n);
        total += (uint64_t)n;
    }
    close(fd);
    free(buf);
    counters("after read");
    printf("reclaimtest: read %llu bytes, %llu bad pages\n", (unsigned long long)total, (unsigned long long)bad);
    CHECK(total == SIZE);
    CHECK(bad == 0);
    CHECK(vm_counter("pgsteal_kswapd") + vm_counter("pgsteal_direct") - steal0 > 0);
    CHECK(vm_counter("pgsteal_kswapd") > 0);
    CHECK(vm_counter("free_lowest") >= vm_counter("watermark_min"));
    if (failures == f0) printf("reclaimtest: read ok\n");
}

static void test_mmap(void) {
    int f0 = failures;
    long long unmapped0 = vm_counter("reclaim_unmapped");
    int fd = open(BIG, O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    const uint64_t *m = mmap(NULL, SIZE, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    close(fd);
    if (m == MAP_FAILED) return;
    /* A private writable mapping of the same file, written: its first page
     * is now the process's own copy, which reclaiming the cache page must
     * leave alone (review of #47) */
    fd = open(BIG, O_RDONLY);
    CHECK(fd >= 0);
    uint64_t *pv = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    CHECK(pv != MAP_FAILED);
    if (pv == MAP_FAILED) return;
    pv[0] = 0xC0FFEEull;
    uint64_t bad = 0;
    for (uint64_t off = 0; off < SIZE; off += CHUNK) bad += check_piece(m + off / 8, off, CHUNK);
    counters("after mmap");
    printf("reclaimtest: mmap %llu bytes, %llu bad pages\n", (unsigned long long)SIZE, (unsigned long long)bad);
    CHECK(bad == 0);
    CHECK(vm_counter("reclaim_unmapped") - unmapped0 > 0);
    /* No watermark check here: a mapped page needs two looks and a TLB
     * shootdown before it can go, so kswapd may fall behind a fast reader
     * and the reader then reclaims for itself below min (allocstall). */
    /* The first pages were unmapped and freed long ago: they fault back in */
    CHECK(check_piece(m, 0, CHUNK) == 0);
    printf("reclaimtest: private copy after the pressure: %#llx %#llx\n", (unsigned long long)pv[0],
           (unsigned long long)pv[1]);
    CHECK(pv[0] == 0xC0FFEEull && pv[1] == 1);   /* the written word, and the file's word 1 */
    CHECK(munmap(pv, 4096) == 0);
    CHECK(munmap((void *)m, SIZE) == 0);
    if (failures == f0) printf("reclaimtest: mmap ok\n");
}

static void test_meminfo(void) {
    int f0 = failures;
    int fd = open("/proc/meminfo", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    CHECK(n > 0);
    text[n > 0 ? n : 0] = 0;
    static const char *rows[] = {"\nCached:", "\nDirty:", "\nWriteback:", "\nActive:", "\nInactive:"};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (strstr(text, rows[i]) == NULL) {
            printf("FAIL reclaimtest: /proc/meminfo has no %s\n", rows[i] + 1);
            failures++;
        }
    }
    if (failures == f0) printf("reclaimtest: meminfo ok\n");
}

int main(void) {
    counters("at start");
    test_idle();
    test_read();
    test_mmap();
    test_meminfo();
    printf("reclaimtest: %d failures\n", failures);
    return failures;
}
