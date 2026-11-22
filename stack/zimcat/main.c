/*
 * zimcat — look inside a Kiwix ZIM archive with our own reader (M3-03a, ADR-0022). Build host only.
 *
 *   zimcat info  ZIM            header, MIME types, metadata, entries per namespace and kind
 *   zimcat ls    ZIM [NS]       one line per entry (of namespace NS): number, namespace, path, MIME or
 *                               "-> target", title if it differs from the path; tab-separated
 *   zimcat cat   ZIM NS/PATH    the entry's data on stdout (redirects followed)
 *   zimcat check ZIM            read and check the whole file once; print what it holds
 *
 * Control characters and backslashes in paths, titles and metadata are printed as \xNN, so each
 * entry stays one line. Exit status: 0 ok, 1 damaged file or entry not found, 2 usage.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arczim.h"

#define META_SHOW_MAX 300 /* bytes of a metadata value printed by `info` */

static void zimcat_usage(void) {
    fputs("usage: zimcat info ZIM | ls ZIM [NS] | cat ZIM NS/PATH | check ZIM\n", stderr);
    exit(2);
}

/* put_escaped: s[0..len) with control characters and backslashes as \xNN. */
static void put_escaped(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || c == '\\') {
            printf("\\x%02x", c);
        } else {
            putchar(c);
        }
    }
}

/* print_ns: one "namespace" line; a namespace byte that isn't printable shows as \xNN. */
static void print_ns(int c, uint64_t count) {
    printf(c >= 0x21 && c < 0x7F ? "namespace %c\t%llu\n" : "namespace \\x%02x\t%llu\n", c, (unsigned long long)count);
}

/* zimcat_fail: print the reader's error and return 1. */
static int zimcat_fail(arczim *z, const char *what) {
    fprintf(stderr, "zimcat: %s: %s\n", what, arczim_error(z));
    return 1;
}

/* is_text: a MIME type whose value `info` prints as text. */
static bool is_text(const char *mime) {
    return strncmp(mime, "text/plain", 10) == 0;
}

static int zimcat_info(arczim *z) {
    const arczim_header *h = arczim_get_header(z);
    printf("version\t%u.%u\nsize\t%llu\nuuid\t", h->major, h->minor, (unsigned long long)arczim_file_size(z));
    for (int i = 0; i < 16; i++) {
        printf("%02x", h->uuid[i]);
    }
    printf("\nentries\t%u\nclusters\t%u\n", h->entry_count, h->cluster_count);
    printf("title_list\t%s\n", h->title_ptr_pos == ARCZIM_NO_TITLE_PTR ? "none" : "present");
    if (h->main_page != ARCZIM_NO_ENTRY) {
        arczim_entry e;
        if (!arczim_entry_get(z, h->main_page, &e)) {
            return zimcat_fail(z, "main page");
        }
        printf("main_page\t%c/", e.ns);
        put_escaped(e.path, e.path_length);
        putchar('\n');
    }
    for (uint16_t i = 0; i < arczim_mime_count(z); i++) {
        printf("mime %u\t%s\n", i, arczim_mime(z, i));
    }
    uint32_t first, end;
    if (!arczim_lower_bound(z, 'M', "", 0, &first) || !arczim_lower_bound(z, 'M' + 1, "", 0, &end)) {
        return zimcat_fail(z, "metadata");
    }
    for (uint32_t i = first; i < end; i++) {
        arczim_entry e;
        const char *value;
        uint64_t size;
        if (!arczim_resolve(z, i, &e) || !arczim_content(z, &e, &value, &size)) {
            return zimcat_fail(z, "metadata");
        }
        fputs("meta ", stdout);
        put_escaped(e.path, e.path_length);
        putchar('\t');
        if (is_text(arczim_mime(z, e.mime))) {
            put_escaped(value, size > META_SHOW_MAX ? META_SHOW_MAX : (size_t)size);
            if (size > META_SHOW_MAX) {
                printf("... (%llu bytes)", (unsigned long long)size);
            }
        } else {
            printf("<%s, %llu bytes>", arczim_mime(z, e.mime), (unsigned long long)size);
        }
        putchar('\n');
    }
    uint64_t per_ns[256] = {0}, kinds[3] = {0};
    for (uint32_t i = 0; i < h->entry_count; i++) {
        arczim_entry e;
        if (!arczim_entry_get(z, i, &e)) {
            return zimcat_fail(z, "entries");
        }
        per_ns[(unsigned char)e.ns]++;
        kinds[e.kind]++;
    }
    for (int c = 0; c < 256; c++) {
        if (per_ns[c]) {
            print_ns(c, per_ns[c]);
        }
    }
    printf("content\t%llu\nredirects\t%llu\nobsolete\t%llu\n", (unsigned long long)kinds[ARCZIM_ENTRY_CONTENT],
           (unsigned long long)kinds[ARCZIM_ENTRY_REDIRECT], (unsigned long long)kinds[ARCZIM_ENTRY_OBSOLETE]);
    return 0;
}

static int zimcat_ls(arczim *z, const char *ns_arg) {
    uint32_t first = 0, end = arczim_entry_count(z);
    if (ns_arg != NULL) {
        char ns = ns_arg[0];
        if (!arczim_lower_bound(z, ns, "", 0, &first) ||
            !arczim_lower_bound(z, (char)((unsigned char)ns + 1), "", 0, &end)) {
            return zimcat_fail(z, "ls");
        }
        if ((unsigned char)ns == 0xFF) {
            end = arczim_entry_count(z);
        }
    }
    for (uint32_t i = first; i < end; i++) {
        arczim_entry e;
        if (!arczim_entry_get(z, i, &e)) {
            return zimcat_fail(z, "ls");
        }
        printf("%u\t", i);
        put_escaped(&e.ns, 1);
        putchar('\t');
        put_escaped(e.path, e.path_length);
        putchar('\t');
        if (e.kind == ARCZIM_ENTRY_CONTENT) {
            fputs(arczim_mime(z, e.mime), stdout);
        } else if (e.kind == ARCZIM_ENTRY_REDIRECT) {
            arczim_entry t;
            if (!arczim_entry_get(z, e.redirect, &t)) {
                return zimcat_fail(z, "ls");
            }
            fputs("-> ", stdout);
            put_escaped(&t.ns, 1);
            putchar('/');
            put_escaped(t.path, t.path_length);
        } else {
            fputs("obsolete", stdout);
        }
        if (e.title != e.path) {
            putchar('\t');
            put_escaped(e.title, e.title_length);
        }
        putchar('\n');
    }
    return 0;
}

static int zimcat_cat(arczim *z, const char *key) {
    uint32_t index;
    arczim_entry e;
    const char *data;
    uint64_t size;
    if (!arczim_find(z, key[0], key + 2, strlen(key + 2), &index) || !arczim_resolve(z, index, &e) ||
        !arczim_content(z, &e, &data, &size)) {
        return zimcat_fail(z, key);
    }
    return fwrite(data, 1, (size_t)size, stdout) == size ? 0 : 1;
}

static int zimcat_check(arczim *z) {
    arczim_stats st;
    if (!arczim_check(z, &st)) {
        arczim_stats_free(&st);
        return zimcat_fail(z, "check");
    }
    printf("entries\t%u\ncontent\t%llu\nredirects\t%llu\nobsolete\t%llu\n", arczim_entry_count(z),
           (unsigned long long)st.content, (unsigned long long)st.redirects, (unsigned long long)st.obsolete);
    for (int c = 0; c < 128; c++) {
        if (st.per_ns[c]) {
            print_ns(c, st.per_ns[c]);
        }
    }
    for (uint16_t i = 0; i < arczim_mime_count(z); i++) {
        printf("mime %s\t%llu entries\t%llu bytes\n", arczim_mime(z, i), (unsigned long long)st.mime[i].entries,
               (unsigned long long)st.mime[i].bytes);
    }
    static const char *const comp_names[16] = {"none(0)", "none", "zlib", "bzip2", "xz", "zstd"};
    printf("clusters\t%u\n", arczim_cluster_count(z));
    for (int c = 0; c < 16; c++) {
        if (st.clusters_by_comp[c]) {
            printf("clusters %s\t%llu\n", comp_names[c] ? comp_names[c] : "unknown",
                   (unsigned long long)st.clusters_by_comp[c]);
        }
    }
    printf("clusters extended\t%llu\ncluster bytes\t%llu\ncluster bytes max\t%llu\n",
           (unsigned long long)st.clusters_extended, (unsigned long long)st.cluster_bytes,
           (unsigned long long)st.cluster_max_bytes);
    printf("blobs\t%llu\nblobs unreferenced\t%llu\nblobs shared\t%llu\n", (unsigned long long)st.blobs,
           (unsigned long long)st.blobs_unreferenced, (unsigned long long)st.blobs_shared);
    printf("html entries\t%llu\n", (unsigned long long)st.html_entries);
    for (int k = 0; k < ARCZIM_LRU_SIZES; k++) {
        printf("html path-order decompressions, cache %u\t%llu\n", arczim_lru_sizes[k],
               (unsigned long long)st.html_lru_misses[k]);
    }
    puts("check\tok");
    arczim_stats_free(&st);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        zimcat_usage();
    }
    const char *cmd = argv[1];
    bool usage_ok = (strcmp(cmd, "info") == 0 && argc == 3) ||
                    (strcmp(cmd, "ls") == 0 && (argc == 3 || (argc == 4 && strlen(argv[3]) == 1))) ||
                    (strcmp(cmd, "cat") == 0 && argc == 4 && argv[3][0] != '\0' && argv[3][1] == '/') ||
                    (strcmp(cmd, "check") == 0 && argc == 3);
    if (!usage_ok) {
        zimcat_usage();
    }
    const char *why;
    arczim *z = arczim_open(argv[2], &why);
    if (z == NULL) {
        fprintf(stderr, "zimcat: %s\n", why);
        return 1;
    }
    int rc;
    if (strcmp(cmd, "info") == 0) {
        rc = zimcat_info(z);
    } else if (strcmp(cmd, "ls") == 0) {
        rc = zimcat_ls(z, argc == 4 ? argv[3] : NULL);
    } else if (strcmp(cmd, "cat") == 0) {
        rc = zimcat_cat(z, argv[3]);
    } else {
        rc = zimcat_check(z);
    }
    arczim_close(z);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fputs("zimcat: write error\n", stderr);
        return 1;
    }
    return rc;
}
