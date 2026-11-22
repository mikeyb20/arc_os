/*
 * packbuild — turns an article stream into a pack's content/ and chunks/ (M3-03, ADR-0011).
 * Stream format: docs/formats/article-stream.md. Pack format: docs/formats/pack-v1.md.
 *
 * Two passes over the stream file. Pass 1 reads the keys (they must be in strictly increasing byte
 * order, so document ID = line number - 1) into one sorted array. Pass 2 reads each document again,
 * rewrites its links from keys to document IDs (binary search; a key that isn't in the stream becomes
 * its link text), cuts it into chunks and adds it with arcpack_writer_add_doc. Any failure stops the
 * build and leaves nothing in the output directory.
 *
 * Chunking (content-lane.md, "Keyword-only first"; tokenizer "words" = arcpack_next_word over the
 * plain text): a heading starts a new chunk, whose section path is the headings above it; a section
 * shorter than min_section words joins the next section's first chunk (the path is the first
 * section's). A section longer than max words is cut into chunks of about target words that overlap
 * by `overlap` words: at the last line end between 3/4 of the target and the target, else at the
 * sentence end nearest the target between 3/4 of the target and max, else after `target` words. Cuts
 * never fall inside a character, a link, a heading line or an image line. A chunk holds at least one
 * word and at most max words; the one exception is a single link, heading or image line that alone
 * holds more than max words, which becomes one chunk.
 *
 * Cost: memory = the key array (key bytes + 8 bytes per document) + one document's line, text and
 * chunk list + the writer's buffers. Link lookup O(log documents); everything else linear in the
 * text. Two reads of the stream.
 */
#ifndef ARCHOS_STACK_PACKBUILD_H
#define ARCHOS_STACK_PACKBUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACKBUILD_LINE_MAX     (64u << 20) /* bytes of one stream line, newline excluded */
#define PACKBUILD_TARGET_WORDS 280u
#define PACKBUILD_MAX_WORDS    360u
#define PACKBUILD_OVERLAP      40u
#define PACKBUILD_MIN_SECTION  20u
#define PACKBUILD_WORDS_HIST   (PACKBUILD_MAX_WORDS + 2) /* last bucket: chunks over max (one long construct) */

/* How documents are cut (the manifest's `chunking`, in words). */
typedef struct {
    uint32_t target;
    uint32_t max;
    uint32_t overlap;
    uint32_t min_section;
} packbuild_chunking;

/* The defaults above (content-lane.md). */
extern const packbuild_chunking packbuild_chunking_default;

/* One chunk of a document: a byte range of its text, its section path (in the chunk list's paths). */
typedef struct {
    uint32_t offset;
    uint32_t length;
    uint32_t words;
    uint32_t section_offset; /* into packbuild_chunks.paths */
    uint32_t section_length;
} packbuild_chunk;

/* A document's chunks; reused from one document to the next (zero-initialise, free with packbuild_chunks_free). */
typedef struct {
    packbuild_chunk *chunk;
    size_t n, cap;
    char *paths; /* section paths, parts separated by ARCPACK_MARK_SEP */
    size_t paths_len, paths_cap;
    struct packbuild_scratch *scratch; /* the chunker's word, line and section arrays */
} packbuild_chunks;

/*
 * packbuild_chunk_text: cut marked-up text[0..len) (pack-v1 markup with links already naming document
 * IDs) into chunks, replacing out's contents. Malformed markup is not an error here (the writer refuses
 * it); the chunker only never reads out of bounds. False only when memory runs out.
 */
bool packbuild_chunk_text(const char *text, size_t len, const packbuild_chunking *opt, packbuild_chunks *out);

/* packbuild_chunks_free: free the arrays. */
void packbuild_chunks_free(packbuild_chunks *c);

/* What a build did. */
typedef struct {
    uint32_t documents;
    uint32_t chunks;
    uint64_t text_bytes;     /* marked-up text after link rewriting */
    uint64_t links;          /* links that name a document of the pack */
    uint64_t links_dropped;  /* links to keys not in the stream, kept as their text */
    uint32_t docs_no_chunks; /* documents without a word */
    uint64_t words_hist[PACKBUILD_WORDS_HIST]; /* chunks by word count */
    uint32_t words_max;                         /* the largest chunk's words (over max: one long construct) */
} packbuild_stats;

/* What to build. */
typedef struct {
    const char *name;    /* pack name (manifest id) */
    const char *version; /* pack version */
    const char *const *tags; /* domain tags in bit order; every chunk carries all of them */
    size_t n_tags;
    packbuild_chunking chunking;
} packbuild_options;

/*
 * packbuild_build: build the pack's content/ and chunks/ in `outdir` (which must not exist or be empty)
 * from the article stream file `stream` (seekable: it is read twice). On failure nothing is left in
 * outdir (a directory packbuild created is removed) and *why says why, naming the stream line (valid
 * until the next call on this thread).
 */
bool packbuild_build(const char *stream, const char *outdir, const packbuild_options *opt, packbuild_stats *st,
                     const char **why);

/* packbuild_words_median: the median words per chunk from st->words_hist (0 if no chunks). */
uint32_t packbuild_words_median(const packbuild_stats *st);

#endif /* ARCHOS_STACK_PACKBUILD_H */
