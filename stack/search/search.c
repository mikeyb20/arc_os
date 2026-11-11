/*
 * search — keyword search over the installed packs from the command line (M3-06a, ADR-0015).
 *
 *   search [--root DIR] [-k N] [--pack NAME] [--] "query"
 *
 * --root  system root holding packs/ (default /)
 * -k      results to print (1-1000, default 10)
 * --pack  search only this pack
 * --      the next argument is the query, even if it starts with '-'
 *
 * Prints one line per result, best first: score, chunk ID, document title and section path
 * (parts joined by " > "), tab-separated. Skipped packs are warned about on stderr. No result
 * is not an error. Never talks to arc-llm: search works with no model installed.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcsearch.h"

#define K_MAX 1000

static void search_usage(void) {
    fputs("usage: search [--root DIR] [-k N] [--pack NAME] [--] \"query\"\n", stderr);
    exit(2);
}

/* print_section: the section path with its separators shown as " > ". */
static void print_section(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ARCPACK_MARK_SEP) {
            fputs(" > ", stdout);
        } else {
            putchar(s[i]);
        }
    }
}

int main(int argc, char **argv) {
    const char *root = "/";
    const char *pack = NULL;
    const char *query = NULL;
    unsigned long k = 10;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            pack = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            k = strtoul(argv[++i], &end, 10);
            if (errno != 0 || *end != '\0' || argv[i][0] == '-' || k < 1 || k > K_MAX) {
                search_usage();
            }
        } else if (query == NULL && strcmp(argv[i], "--") == 0 && i + 1 < argc) {
            query = argv[++i];
        } else if (query == NULL && (argv[i][0] != '-' || argv[i][1] == '\0')) {
            query = argv[i];
        } else {
            search_usage();
        }
    }
    if (query == NULL) {
        search_usage();
    }
    const char *why;
    arcsearch *s = arcsearch_open(root, &why);
    if (s == NULL) {
        fprintf(stderr, "search: %s\n", why);
        return 1;
    }
    for (size_t i = 0; i < arcsearch_warning_count(s); i++) {
        fprintf(stderr, "search: warning: %s\n", arcsearch_warning(s, i));
    }
    static arcsearch_result results[K_MAX];
    size_t n;
    if (!arcsearch_search(s, query, pack, k, results, &n, &why)) {
        fprintf(stderr, "search: %s\n", why);
        arcsearch_close(s);
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        const arcsearch_result *r = &results[i];
        printf("%.4f\t%s\t%.*s\t", r->score, r->id, (int)r->title_length, r->title);
        print_section(r->section, r->section_length);
        putchar('\n');
    }
    arcsearch_close(s);
    return 0;
}
