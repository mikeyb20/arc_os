/*
 * arcsearch — keyword search over the installed packs (M3-06a, ADR-0011, ADR-0015). Index format:
 * docs/formats/pack-v1.md, "fts/index.sqlite".
 *
 * Each pack's chunks are indexed in a contentless SQLite FTS5 table (title, section path, plain
 * body text; rowid = global chunk index). arcsearch_search() turns the query into quoted words
 * joined by OR, asks every pack for its best k by bm25, and merges the lists into the top k.
 * Nothing here talks to arc-llm: search works with no model installed (ADR-0015).
 *
 * Cost: one FTS5 query per pack, proportional to the matching postings, never to the pack's
 * chunk count; the merge is O(k x packs) insertions into k slots. Memory: SQLite's page cache
 * per open index (its default) and the caller's k results. Indexes open immutable: no locks,
 * no journal files.
 */
#ifndef ARCHOS_STACK_ARCSEARCH_H
#define ARCHOS_STACK_ARCSEARCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arcpack.h"

#define ARCSEARCH_FTS_SCHEMA_VERSION 1
#define ARCSEARCH_MAX_PACKS          256u /* packs one arcsearch_open() takes; more are skipped with a warning */
#define ARCSEARCH_MAX_WARNINGS       32u

typedef struct arcsearch arcsearch;

/* One hit. Strings point into the pack's mapped files and stay valid until arcsearch_close(). */
typedef struct {
    char id[ARCPACK_CHUNK_ID_MAX]; /* "pack:doc:n", NUL-terminated */
    double score;                  /* -bm25: higher is better; comparable only roughly across packs */
    uint32_t chunk;                /* global chunk index in its pack */
    const char *pack;              /* pack name, NUL-terminated */
    const char *title;             /* document title, title_length bytes */
    size_t title_length;
    const char *section; /* section path, parts separated by ARCPACK_MARK_SEP */
    size_t section_length;
} arcsearch_result;

/*
 * arcsearch_build_fts: write `pack_dir`/fts/index.sqlite from the pack in pack_dir (its
 * manifest.json must exist: name, version and chunk_count come from it), in one transaction,
 * into a temporary file renamed into place. Used by tests now and the pack pipeline (M3-04).
 * False with *why (valid until the next call on this thread) on failure.
 */
bool arcsearch_build_fts(const char *pack_dir, const char **why);

/*
 * arcsearch_open: open every installed pack under `root`/packs/<name>/<highest version>/
 * with its index, read-only. A pack that can't be opened, has no index, or whose index
 * `meta` doesn't match its manifest (name, version, chunk count) is skipped with a warning.
 * NULL with *why only if root/packs can't be read or memory runs out; no packs is not an error.
 */
arcsearch *arcsearch_open(const char *root, const char **why);

/* arcsearch_close: close every pack; NULL is fine. */
void arcsearch_close(arcsearch *s);

/* Packs open, and the warnings arcsearch_open() gave (NUL-terminated, at most ARCSEARCH_MAX_WARNINGS). */
size_t arcsearch_pack_count(const arcsearch *s);
const char *arcsearch_pack_name(const arcsearch *s, size_t i);
size_t arcsearch_warning_count(const arcsearch *s);
const char *arcsearch_warning(const arcsearch *s, size_t i);

/*
 * arcsearch_search: the best k chunks for `query` (UTF-8) over all packs, or only pack `pack`
 * when it is not NULL, best first, into results[0..k); *n = how many. The query is only words:
 * every run of Unicode letters and digits becomes a quoted word (FTS5 syntax in it is never
 * syntax), English stopwords are dropped, and the words are joined with OR. A query with no
 * words left gives 0 results, not an error. False with *why (static or valid until the next
 * call on `s`) on an SQLite or pack error.
 */
bool arcsearch_search(arcsearch *s, const char *query, const char *pack, size_t k, arcsearch_result *results, size_t *n,
                      const char **why);

/*
 * arcsearch_match_expr: the FTS5 MATCH expression arcsearch_search() uses for `query`, into
 * buf (NUL-terminated); returns its length, 0 if no words are left (or cap is too small).
 * Exposed for tests.
 */
size_t arcsearch_match_expr(const char *query, char *buf, size_t cap);

#endif /* ARCHOS_STACK_ARCSEARCH_H */
