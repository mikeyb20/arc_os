/*
 * arczim_test — lane test for the ZIM reader (M3-03a). Builds ZIMs with the test writer (zim_fixture.h)
 * into a temporary directory and checks:
 *   - a good v6 file: header, MIME list, every entry by number and by path, titles, redirect chains,
 *     content of zstd, uncompressed and extended clusters, metadata, lower bounds, misses, the check pass
 *   - the cluster cache (one slot, LRU reuse) and the cluster size limit
 *   - a v5 file (old namespaces), and an extended cluster in a v5 file refused
 *   - xz, zlib and unknown compression refused with an error naming the cluster
 *   - damaged headers refused by arczim_open; damaged dirents, pointer lists, clusters and blob tables
 *     refused by the call that reads them; a redirect loop and out-of-order paths found by arczim_check
 *   - FUZZ_ROUNDS (1500) randomly damaged copies (bytes flipped, file cut short): every call returns, none crashes
 * Exit status 0 when every check passes (lane_check.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../lane_check.h"
#include "arczim.h"
#include "zim_fixture.h"

#ifndef FUZZ_ROUNDS
#define FUZZ_ROUNDS 1500 /* a sanitizer run can raise it: -DFUZZ_ROUNDS=50000 */
#endif

static char tmpdir[] = "arczim_test.XXXXXX";
static char tmpfile_path[64];

static const char png[] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR binary with NULs";
static char big[4096];

/* The good file: 3 clusters (zstd, none, zstd extended), 12 entries. */
static const zimfx_cluster clusters[] = {
    {ARCZIM_COMP_ZSTD, false},
    {ARCZIM_COMP_NONE, false},
    {ARCZIM_COMP_ZSTD, true},
};

static zimfx_entry entries[] = {
    {'M', "Title", NULL, "text/plain", "Test ZIM", 8, 0, NULL},
    {'M', "License", NULL, "text/plain", "CC-BY-SA-4.0", 12, 0, NULL},
    {'C', "Alpha", "Alpha title", "text/html", "<h1>Alpha</h1><p>First.</p>", 27, 0, NULL},
    {'C', "Beta", NULL, "text/html", big, sizeof(big), 2, NULL},
    {'C', "Gamma", NULL, NULL, NULL, 0, 0, "C/Alpha"},
    {'C', "Delta", NULL, NULL, NULL, 0, 0, "C/Gamma"},
    {'C', "img.png", NULL, "image/png", png, sizeof(png) - 1, 1, NULL},
    {'C', "empty", NULL, "text/plain", "", 0, 1, NULL},
    {'C', "caf\xc3\xa9", NULL, "text/html; charset=utf-8", "<p>Caf\xc3\xa9</p>", 13, 0, NULL},
    {'C', "dir/Alpha", NULL, "text/html", "<p>nested</p>", 13, 2, NULL},
    {'W', "mainPage", NULL, NULL, NULL, 0, 0, "C/Alpha"},
    {'X', "listing/titleOrdered/v1", NULL, "application/octet-stream+zimlisting", "\2\0\0\0", 4, 1, NULL},
};
#define N_ENTRIES (sizeof(entries) / sizeof(entries[0]))

static zimfx_spec good_spec(void) {
    return (zimfx_spec){6, 3, entries, N_ENTRIES, clusters, 3, true, "W/mainPage"};
}

/* build: the ZIM for spec (must succeed). */
static uint8_t *build(const zimfx_spec *s, size_t *len, zimfx_layout *lay) {
    uint8_t *buf;
    char err[256];
    if (!zimfx_build(s, &buf, len, lay, err, sizeof(err))) {
        fprintf(stderr, "fixture: %s\n", err);
        exit(1);
    }
    return buf;
}

/* open_buf: write buf to the temporary file and open it; NULL (and *why) if arczim_open refuses. */
static arczim *open_buf(const uint8_t *buf, size_t len, const char **why) {
    if (!zimfx_write_file(tmpfile_path, buf, len)) {
        perror("write fixture");
        exit(1);
    }
    return arczim_open(tmpfile_path, why);
}

static void patch16(uint8_t *b, uint64_t at, uint16_t v) {
    memcpy(b + at, &v, 2);
}
static void patch32(uint8_t *b, uint64_t at, uint32_t v) {
    memcpy(b + at, &v, 4);
}
static void patch64(uint8_t *b, uint64_t at, uint64_t v) {
    memcpy(b + at, &v, 8);
}

static bool error_has(arczim *z, const char *needle) {
    return strstr(arczim_error(z), needle) != NULL;
}

/* content_is: entry by path resolves to content equal to data[0..size). */
static bool content_is(arczim *z, char ns, const char *path, const char *data, size_t size) {
    uint32_t i;
    arczim_entry e;
    const char *got;
    uint64_t n;
    return arczim_find(z, ns, path, strlen(path), &i) && arczim_resolve(z, i, &e) && arczim_content(z, &e, &got, &n) &&
           n == size && memcmp(got, data, size) == 0;
}

/* test_check_stats: arczim_check on the good file and what it counts. */
static void test_check_stats(arczim *z) {
    arczim_stats st;
    bool ok = arczim_check(z, &st);
    lane_check(ok, "check passes on the good file");
    lane_check(st.content == 9 && st.redirects == 3 && st.obsolete == 0 && st.per_ns['C'] == 8 && st.per_ns['M'] == 2,
               "check counts entries by kind and namespace");
    lane_check(st.clusters_by_comp[ARCZIM_COMP_ZSTD] == 2 && st.clusters_by_comp[ARCZIM_COMP_NONE] == 1 &&
                   st.clusters_extended == 1 && st.blobs == 9 && st.blobs_unreferenced == 0,
               "check counts clusters and blobs");
    lane_check(st.html_entries == 4 && st.mime[1].entries == 3 && st.mime[1].bytes == 27 + sizeof(big) + 13,
               "check counts html entries and bytes per MIME type");
    lane_check(st.html_lru_misses[0] >= 2 && st.html_lru_misses[3] == 2, "path-order locality numbers");
    arczim_stats_free(&st);
}

/* test_cache: one cache slot, then the cluster size limit, on the good file. */
static void test_cache(arczim *z) {
    lane_check(arczim_set_cache_slots(z, 1) && !arczim_set_cache_slots(z, 0) && !arczim_set_cache_slots(z, 65),
               "cache slot limits");
    lane_check(content_is(z, 'C', "Alpha", entries[2].data, entries[2].size) &&
                   content_is(z, 'C', "Beta", big, sizeof(big)) &&
                   content_is(z, 'C', "Alpha", entries[2].data, entries[2].size),
               "one cache slot: clusters re-read correctly");
    lane_check(arczim_set_cluster_max(z, 1024) && !content_is(z, 'C', "Beta", big, sizeof(big)) &&
                   error_has(z, "limit"),
               "cluster over the size limit refused");
    lane_check(content_is(z, 'C', "Alpha", entries[2].data, entries[2].size),
               "small cluster still read under the limit");
    lane_check(!arczim_set_cluster_max(z, 10), "cluster limit below 64 refused");
}

static void test_good(void) {
    size_t len;
    zimfx_layout lay;
    zimfx_spec s = good_spec();
    uint8_t *buf = build(&s, &len, &lay);
    const char *why = NULL;
    arczim *z = open_buf(buf, len, &why);
    lane_check(z != NULL, "good file opens");
    if (z == NULL) {
        fprintf(stderr, "  %s\n", why);
        exit(1);
    }
    const arczim_header *h = arczim_get_header(z);
    lane_check(h->major == 6 && h->minor == 3 && h->uuid[0] == 0xA0, "header version and uuid");
    lane_check(arczim_entry_count(z) == N_ENTRIES && arczim_cluster_count(z) == 3, "entry and cluster counts");
    lane_check(arczim_file_size(z) == len && h->checksum_pos == len - 16, "file size and checksum position");
    lane_check(arczim_mime_count(z) == 5 && strcmp(arczim_mime(z, 0), "text/plain") == 0 &&
                   strcmp(arczim_mime(z, 3), "text/html; charset=utf-8") == 0 && arczim_mime(z, 5) == NULL,
               "MIME list in order, out of range is NULL");

    bool by_number = true, by_path = true, data_ok = true;
    for (size_t i = 0; i < N_ENTRIES; i++) {
        const zimfx_entry *x = &entries[i];
        arczim_entry e;
        uint32_t at;
        by_number &= arczim_entry_get(z, lay.entry_of[i], &e) && e.ns == x->ns && e.path_length == strlen(x->path) &&
                     memcmp(e.path, x->path, e.path_length) == 0 && e.path[e.path_length] == '\0' &&
                     e.kind == (x->redirect ? ARCZIM_ENTRY_REDIRECT : ARCZIM_ENTRY_CONTENT);
        const char *title = x->title ? x->title : x->path;
        by_number &= e.title_length == strlen(title) && memcmp(e.title, title, e.title_length) == 0;
        by_path &= arczim_find(z, x->ns, x->path, strlen(x->path), &at) && at == lay.entry_of[i];
        if (!x->redirect) {
            data_ok &= content_is(z, x->ns, x->path, x->data, x->size);
        }
    }
    lane_check(by_number, "every entry by number: namespace, path, title, kind");
    lane_check(by_path, "every entry found by path");
    lane_check(data_ok, "every content entry's data (zstd, uncompressed, extended clusters, empty blob)");
    for (uint32_t i = 1; i < N_ENTRIES; i++) {
        arczim_entry a, b;
        arczim_entry_get(z, i - 1, &a);
        arczim_entry_get(z, i, &b);
        if (a.ns > b.ns || (a.ns == b.ns && strcmp(a.path, b.path) >= 0)) {
            lane_check(false, "entries numbered in (namespace, path) order");
        }
    }

    arczim_entry e;
    lane_check(content_is(z, 'C', "Delta", entries[2].data, entries[2].size), "redirect chain Delta -> Gamma -> Alpha");
    lane_check(arczim_resolve(z, lay.entry_of[5], &e) && e.index == lay.entry_of[2], "resolve gives the content entry");
    lane_check(!arczim_content(z, &(arczim_entry){.kind = ARCZIM_ENTRY_REDIRECT}, &(const char *){0}, &(uint64_t){0}),
               "content of a redirect entry is refused");
    lane_check(h->main_page == lay.entry_of[10], "main page is W/mainPage");

    const char *v;
    uint64_t n;
    lane_check(arczim_metadata(z, "Title", &v, &n) && n == 8 && memcmp(v, "Test ZIM", 8) == 0, "metadata Title");
    lane_check(arczim_metadata(z, "License", &v, &n) && n == 12, "metadata License");
    lane_check(!arczim_metadata(z, "Creator", &v, &n) && error_has(z, "not found"), "missing metadata: not found");

    uint32_t at;
    const char *misses[] = {"Alph", "Alpha2", "Zzz", "", "dir", "dir/Alpha/x"};
    bool miss_ok = true;
    for (size_t i = 0; i < sizeof(misses) / sizeof(misses[0]); i++) {
        miss_ok &= !arczim_find(z, 'C', misses[i], strlen(misses[i]), &at) && error_has(z, "not found");
    }
    miss_ok &= !arczim_find(z, 'A', "Alpha", 5, &at) && !arczim_find(z, 'Z', "x", 1, &at) &&
               !arczim_find(z, '\0', "", 0, &at) && !arczim_find(z, (char)0xFF, "", 0, &at);
    lane_check(miss_ok, "paths and namespaces that aren't there are not found");
    uint32_t m0, m1;
    lane_check(arczim_lower_bound(z, 'M', "", 0, &m0) && arczim_lower_bound(z, 'N', "", 0, &m1) && m0 == 8 && m1 == 10,
               "lower bound brackets the M namespace");
    lane_check(arczim_lower_bound(z, (char)0xFF, "", 0, &m1) && m1 == N_ENTRIES, "lower bound past the end");
    lane_check(!arczim_entry_get(z, N_ENTRIES, &e), "entry number out of range refused");
    lane_check(!arczim_blob(z, 3, 0, &v, &n) && !arczim_blob(z, 0, 99, &v, &n), "cluster or blob out of range refused");

    test_check_stats(z);
    test_cache(z);
    arczim_close(z);
    zimfx_layout_free(&lay);
    free(buf);
}

static void test_v5(void) {
    static const zimfx_cluster c5[] = {{ARCZIM_COMP_ZSTD, false}, {ARCZIM_COMP_NONE, true}};
    static const zimfx_entry e5[] = {
        {'A', "Old", NULL, "text/html", "<p>old</p>", 10, 0, NULL},
        {'I', "pic.png", NULL, "image/png", "PNG", 3, 1, NULL},
        {'M', "Title", NULL, "text/plain", "v5", 2, 0, NULL},
    };
    zimfx_spec s = {5, 0, e5, 3, c5, 2, true, "A/Old"};
    size_t len;
    uint8_t *buf = build(&s, &len, NULL);
    const char *why;
    arczim *z = open_buf(buf, len, &why);
    lane_check(z != NULL && content_is(z, 'A', "Old", "<p>old</p>", 10), "v5 file with old namespaces reads");
    const char *v;
    uint64_t n;
    lane_check(z && !arczim_blob(z, 1, 0, &v, &n) && error_has(z, "version 5"),
               "extended cluster in a v5 file refused");
    arczim_close(z);
    free(buf);
}

/* comp_refused: a file whose only cluster has compression `code`: reading and checking it fail. */
static void comp_refused(uint8_t code, const char *needle, const char *what) {
    zimfx_cluster c = {code, false};
    zimfx_entry x = {'C', "A", NULL, "text/html", "abc", 3, 0, NULL};
    zimfx_spec s = {6, 3, &x, 1, &c, 1, false, NULL};
    size_t len;
    uint8_t *buf = build(&s, &len, NULL);
    const char *why;
    arczim *z = open_buf(buf, len, &why);
    arczim_stats st;
    bool refused = z && !content_is(z, 'C', "A", "abc", 3) && error_has(z, needle) && error_has(z, "cluster 0");
    refused = refused && !arczim_check(z, &st) && error_has(z, needle);
    arczim_stats_free(&st);
    lane_check(refused, what);
    arczim_close(z);
    free(buf);
}

/* open_refused: the good file patched by `fn` is refused by arczim_open with `needle` in the reason. */
static void open_refused(void (*fn)(uint8_t *, size_t *, const zimfx_layout *), const char *needle, const char *what) {
    size_t len;
    zimfx_layout lay;
    zimfx_spec s = good_spec();
    uint8_t *buf = build(&s, &len, &lay);
    fn(buf, &len, &lay);
    const char *why = "";
    arczim *z = open_buf(buf, len, &why);
    lane_check(z == NULL && strstr(why, needle) != NULL, what);
    if (z != NULL || strstr(why, needle) == NULL) {
        fprintf(stderr, "  got: %s\n", z ? "opened" : why);
    }
    arczim_close(z);
    zimfx_layout_free(&lay);
    free(buf);
}

static void bad_magic(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len, (void)l;
    b[0] = 'X';
}
static void bad_major(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len, (void)l;
    patch16(b, 4, 7);
}
static void cut_header(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)b, (void)l;
    *len = 79;
}
static void cut_checksum(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)b, (void)l;
    *len -= 1;
}
static void bad_checksum_pos(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)l;
    patch64(b, 72, *len);
}
static void bad_mime_pos(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len, (void)l;
    patch64(b, 56, 40);
}
static void mime_unterminated(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len;
    memset(b + l->mime_list_pos, 'x', l->checksum_pos - l->mime_list_pos);
}
static void huge_entry_count(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len, (void)l;
    patch32(b, 24, 0xFFFFFFFFu);
}
static void path_list_past_end(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len;
    patch64(b, 32, l->checksum_pos - 8);
}
static void cluster_list_past_end(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len, (void)l;
    patch64(b, 48, UINT64_MAX - 4);
}
static void bad_main_page(uint8_t *b, size_t *len, const zimfx_layout *l) {
    (void)len, (void)l;
    patch32(b, 64, N_ENTRIES);
}

static void test_open_refused(void) {
    open_refused(bad_magic, "bad magic", "bad magic number refused");
    open_refused(bad_major, "major version 7", "major version 7 refused");
    open_refused(cut_header, "shorter than a ZIM header", "file shorter than the header refused");
    open_refused(cut_checksum, "checksum position", "file cut inside the checksum refused");
    open_refused(bad_checksum_pos, "checksum position", "checksum position at the end refused");
    open_refused(bad_mime_pos, "MIME list position", "MIME list inside the header refused");
    open_refused(mime_unterminated, "not terminated", "unterminated MIME list refused");
    open_refused(huge_entry_count, "path pointer list", "entry count past the file refused");
    open_refused(path_list_past_end, "path pointer list", "path pointer list past the data refused");
    open_refused(cluster_list_past_end, "cluster pointer list", "cluster pointer list offset overflow refused");
    open_refused(bad_main_page, "main page", "main page out of range refused");
}

/* with_damage: open the good file patched by fn, run `probe`, close. */
static void with_damage(void (*fn)(uint8_t *, const zimfx_layout *), bool (*probe)(arczim *), const char *what) {
    size_t len;
    zimfx_layout lay;
    zimfx_spec s = good_spec();
    uint8_t *buf = build(&s, &len, &lay);
    fn(buf, &lay);
    const char *why;
    arczim *z = open_buf(buf, len, &why);
    bool ok = z != NULL && probe(z);
    lane_check(ok, what);
    if (!ok) {
        fprintf(stderr, "  error: %s\n", z ? arczim_error(z) : why);
    }
    arczim_close(z);
    zimfx_layout_free(&lay);
    free(buf);
}

/* Entry numbers in the good file (path order): 0 C/Alpha 1 C/Beta 2 C/Delta 3 C/Gamma 4 C/caf\xc3\xa9
 * 5 C/dir/Alpha 6 C/empty 7 C/img.png 8 M/License 9 M/Title 10 W/mainPage 11 X/listing. */
static void dirent_outside(uint8_t *b, const zimfx_layout *l) {
    patch64(b, l->path_ptr_pos + 1 * 8, l->checksum_pos - 4);
}
static void dirent_bad_mime(uint8_t *b, const zimfx_layout *l) {
    patch16(b, l->dirent_pos[1], 200);
}
static void dirent_bad_cluster(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->dirent_pos[1] + 8, 99);
}
static void dirent_bad_blob(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->dirent_pos[1] + 12, 50);
}
static void dirent_bad_redirect(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->dirent_pos[3] + 8, 9999);
}
static void redirect_loop(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->dirent_pos[3] + 8, 2); /* Gamma -> Delta, and Delta -> Gamma already */
}
static void dirent_unterminated(uint8_t *b, const zimfx_layout *l) {
    uint64_t at = l->checksum_pos - 20; /* inside the cluster pointer list */
    patch16(b, at, ARCZIM_MIME_REDIRECT);
    b[at + 2] = 0;
    b[at + 3] = 'C';
    patch32(b, at + 4, 0);
    patch32(b, at + 8, 0);
    memset(b + at + 12, 'x', 8);
    patch64(b, l->path_ptr_pos + 1 * 8, at);
}
static void swap_paths(uint8_t *b, const zimfx_layout *l) {
    patch64(b, l->path_ptr_pos + 0 * 8, l->dirent_pos[1]);
    patch64(b, l->path_ptr_pos + 1 * 8, l->dirent_pos[0]);
}
static void repeat_path(uint8_t *b, const zimfx_layout *l) {
    patch64(b, l->path_ptr_pos + 1 * 8, l->dirent_pos[0]);
}

static bool get1_fails(arczim *z) {
    arczim_entry e;
    return !arczim_entry_get(z, 1, &e) && error_has(z, "entry 1");
}
static bool beta_fails(arczim *z) {
    return !content_is(z, 'C', "Beta", big, sizeof(big));
}
static bool gamma_fails(arczim *z) {
    arczim_entry e;
    return !arczim_resolve(z, 3, &e);
}
static bool loop_found(arczim *z) {
    arczim_entry e;
    arczim_stats st;
    bool ok = !arczim_resolve(z, 3, &e) && error_has(z, "redirects") && !arczim_check(z, &st);
    arczim_stats_free(&st);
    return ok;
}
static bool order_found(arczim *z) {
    arczim_stats st;
    bool ok = !arczim_check(z, &st) && error_has(z, "path order");
    arczim_stats_free(&st);
    return ok;
}
static bool blob_check_fails(arczim *z) {
    arczim_stats st;
    bool ok = beta_fails(z) && error_has(z, "blob 50") && !arczim_check(z, &st);
    arczim_stats_free(&st);
    return ok;
}

/* Cluster damage. Cluster 0 zstd, 1 uncompressed (blobs: img.png, empty, listing), 2 zstd extended. */
static void cluster_ptr_outside(uint8_t *b, const zimfx_layout *l) {
    patch64(b, l->cluster_ptr_pos + 1 * 8, l->checksum_pos);
}
static void cluster_bad_flags(uint8_t *b, const zimfx_layout *l) {
    b[l->cluster_pos[1]] |= 0x40;
}
static void cluster_first_offset_odd(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->cluster_pos[1] + 1, 3);
}
static void cluster_first_offset_huge(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->cluster_pos[1] + 1, 0xFFFFFFF0u);
}
static void cluster_offsets_backwards(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->cluster_pos[1] + 1 + 4, 0); /* blob 1 starts before the table ends */
}
static void cluster_offset_past_end(uint8_t *b, const zimfx_layout *l) {
    patch32(b, l->cluster_pos[1] + 1 + 12, 0x7FFFFFFFu); /* end of the last blob */
}
static void zstd_garbage(uint8_t *b, const zimfx_layout *l) {
    memset(b + l->cluster_pos[2] + 1, 0xFF, 8);
}
static void zstd_cut(uint8_t *b, const zimfx_layout *l) {
    /* Corrupt the middle of cluster 2's frame. The fixture's frames carry zstd's content checksum, which
     * catches this; libzim's frames have none, so there only zstd's structural checks can (ADR-0022). */
    uint64_t mid = l->cluster_pos[2] + (l->dirent_pos[0] - l->cluster_pos[2]) / 2;
    memset(b + mid, 0xA5, 16);
}

static bool img_fails(arczim *z) {
    return !content_is(z, 'C', "img.png", png, sizeof(png) - 1) && error_has(z, "cluster 1");
}
static bool img_or_check_fails(arczim *z) {
    arczim_stats st;
    bool ok = !arczim_check(z, &st) && error_has(z, "cluster 1");
    arczim_stats_free(&st);
    return ok;
}
static bool beta_zstd_fails(arczim *z) {
    return beta_fails(z) && error_has(z, "cluster 2");
}

static void test_damaged(void) {
    with_damage(dirent_outside, get1_fails, "dirent pointer past the data refused");
    with_damage(dirent_bad_mime, get1_fails, "dirent MIME number out of range refused");
    with_damage(dirent_bad_cluster, get1_fails, "dirent cluster number out of range refused");
    with_damage(dirent_bad_blob, blob_check_fails, "dirent blob number out of range refused (read and check)");
    with_damage(dirent_bad_redirect, gamma_fails, "redirect to an entry out of range refused");
    with_damage(redirect_loop, loop_found, "redirect loop found (resolve and check)");
    with_damage(dirent_unterminated, get1_fails, "dirent path without its NUL refused");
    with_damage(swap_paths, order_found, "entries out of path order found by check");
    with_damage(repeat_path, order_found, "repeated path found by check");
    with_damage(cluster_ptr_outside, img_fails, "cluster pointer past the data refused");
    with_damage(cluster_bad_flags, img_fails, "cluster info byte with unknown flags refused");
    with_damage(cluster_first_offset_odd, img_fails, "blob table first offset not a multiple refused");
    with_damage(cluster_first_offset_huge, img_fails, "blob table first offset past the data refused");
    with_damage(cluster_offsets_backwards, img_or_check_fails, "blob offsets going backwards refused");
    with_damage(cluster_offset_past_end, img_or_check_fails, "blob offset past the data refused");
    with_damage(zstd_garbage, beta_zstd_fails, "zstd cluster that is not a frame refused");
    with_damage(zstd_cut, beta_zstd_fails, "damaged zstd frame refused");
}

/* exercise: call everything on a possibly damaged file; the result doesn't matter, returning does. */
static void exercise(arczim *z) {
    const char *v;
    uint64_t n;
    arczim_stats st;
    for (uint32_t i = 0; i < arczim_entry_count(z) && i < 64; i++) {
        arczim_entry e;
        if (arczim_entry_get(z, i, &e) && arczim_resolve(z, i, &e)) {
            arczim_content(z, &e, &v, &n);
        }
    }
    for (size_t i = 0; i < N_ENTRIES; i++) {
        uint32_t at;
        arczim_find(z, entries[i].ns, entries[i].path, strlen(entries[i].path), &at);
    }
    for (uint32_t c = 0; c < arczim_cluster_count(z) && c < 8; c++) {
        arczim_blob(z, c, 0, &v, &n);
    }
    arczim_metadata(z, "Title", &v, &n);
    arczim_check(z, &st);
    arczim_stats_free(&st);
}

static void test_fuzz(void) {
    size_t len;
    zimfx_spec s = good_spec();
    uint8_t *good = build(&s, &len, NULL);
    uint8_t *buf = malloc(len);
    unsigned opened = 0, checked = 0;
    srand(20261122);
    for (int round = 0; round < FUZZ_ROUNDS; round++) {
        memcpy(buf, good, len);
        size_t n = len;
        if (round % 10 == 9) {
            n = (size_t)rand() % len; /* cut short */
        } else {
            for (int k = 1 + rand() % 6; k > 0; k--) {
                buf[(size_t)rand() % len] ^= (uint8_t)(1 + rand() % 255);
            }
        }
        const char *why;
        arczim *z = open_buf(buf, n, &why);
        if (z != NULL) {
            opened++;
            arczim_stats st;
            checked += arczim_check(z, &st);
            arczim_stats_free(&st);
            exercise(z);
            arczim_close(z);
        }
    }
    printf("fuzz: %d damaged copies, %u opened, %u passed check\n", FUZZ_ROUNDS, opened, checked);
    lane_check(opened > 0 && checked < opened, "randomly damaged copies: every call returned");
    free(buf);
    free(good);
}

int main(void) {
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (char)('a' + i % 23);
    }
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(tmpfile_path, sizeof(tmpfile_path), "%s/t.zim", tmpdir);
    test_good();
    test_v5();
    comp_refused(ARCZIM_COMP_XZ, "xz", "xz cluster refused (read and check)");
    comp_refused(ARCZIM_COMP_ZLIB, "removed", "zlib cluster refused");
    comp_refused(9, "unknown compression", "unknown compression code refused");
    test_open_refused();
    test_damaged();
    test_fuzz();
    unlink(tmpfile_path);
    rmdir(tmpdir);
    return lane_check_done("arczim");
}
