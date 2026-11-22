/*
 * packbuild — the pack pipeline's C half (M3-03; content-lane.md). Build host now, arc_os later.
 *
 *   packbuild build --name N --version V [--tags a,b] STREAM OUTDIR
 *
 * Reads the article stream STREAM (docs/formats/article-stream.md) and writes OUTDIR/content/ and
 * OUTDIR/chunks/ (docs/formats/pack-v1.md). OUTDIR must not exist or be empty; on failure nothing is
 * left in it. Every chunk carries every tag in --tags (the manifest's domain_tags, in bit order).
 * Prints what it built: documents, chunks, words per chunk, links. Exit status: 0 built, 1 failed, 2 usage.
 * (The FTS index, manifest.json and the package come from M3-04.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcpack.h"
#include "packbuild.h"

static void packbuild_usage(void) {
    fputs("usage: packbuild build --name N --version V [--tags a,b] STREAM OUTDIR\n", stderr);
    exit(2);
}

/* split_tags: "a,b,c" into tags[] (pointers into s, which is modified); -1 if more than max or one is empty. */
static int split_tags(char *s, const char **tags, int max) {
    int n = 0;
    if (*s == '\0') {
        return 0;
    }
    for (char *p = s;; p++) {
        char *comma = strchr(p, ',');
        if (n == max || comma == p || *p == '\0') {
            return -1;
        }
        tags[n++] = p;
        if (comma == NULL) {
            return n;
        }
        *comma = '\0';
        p = comma;
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "build") != 0) {
        packbuild_usage();
    }
    const char *tags[ARCPACK_MAX_TAGS];
    packbuild_options opt = {.tags = tags, .chunking = packbuild_chunking_default};
    char *tag_arg = NULL;
    int i = 2;
    for (; i + 1 < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        if (strcmp(argv[i], "--name") == 0) {
            opt.name = argv[i + 1];
        } else if (strcmp(argv[i], "--version") == 0) {
            opt.version = argv[i + 1];
        } else if (strcmp(argv[i], "--tags") == 0) {
            tag_arg = argv[i + 1];
        } else {
            packbuild_usage();
        }
    }
    if (opt.name == NULL || opt.version == NULL || argc - i != 2) {
        packbuild_usage();
    }
    int n = tag_arg ? split_tags(tag_arg, tags, ARCPACK_MAX_TAGS) : 0;
    if (n < 0) {
        fputs("packbuild: --tags: empty tag or more than 64\n", stderr);
        return 2;
    }
    opt.n_tags = (size_t)n;
    packbuild_stats st;
    const char *why;
    if (!packbuild_build(argv[i], argv[i + 1], &opt, &st, &why)) {
        fprintf(stderr, "packbuild: %s\n", why);
        return 1;
    }
    uint32_t min = 0;
    for (uint32_t w = 0; w < PACKBUILD_WORDS_HIST && min == 0; w++) {
        min = st.words_hist[w] ? w : 0;
    }
    printf("documents\t%u\nchunks\t%u\ndocuments without a chunk\t%u\ntext bytes\t%llu\n", st.documents, st.chunks,
           st.docs_no_chunks, (unsigned long long)st.text_bytes);
    printf("words per chunk\tmin %u\tmedian %u\tmax %u\n", min, packbuild_words_median(&st), st.words_max);
    printf("chunks over the maximum (one long link, heading or image line)\t%llu\n",
           (unsigned long long)st.words_hist[PACKBUILD_WORDS_HIST - 1]);
    printf("links\t%llu\nlinks to keys not in the stream (kept as text)\t%llu\n", (unsigned long long)st.links,
           (unsigned long long)st.links_dropped);
    return fflush(stdout) == 0 ? 0 : 1;
}
