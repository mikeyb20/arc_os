/*
 * arcsearch.c — finding the installed packs, opening their indexes, and keyword search (arcsearch.h).
 *
 * arcsearch_open() lists <root>/packs/, picks each pack's highest version (lane-l.md), opens the
 * pack with libarcpack and its index read-only and immutable, and checks the index's meta rows
 * against the pack. Each pack keeps one prepared query. A search binds the MATCH expression and
 * k, steps through each pack's best k and keeps the overall best k in the caller's array.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wctype.h>

#include "arcjson.h"
#include "arcsearch.h"

#define EXPR_MAX 4096 /* bytes of MATCH expression; words past it are dropped */

/* The query: rows by bm25 with the title weighted 5, the section path 2 and the body 1. */
static const char QUERY[] =
    "SELECT rowid, bm25(fts, 5.0, 2.0, 1.0) AS r FROM fts WHERE fts MATCH ?1 ORDER BY r LIMIT ?2";

/* Words a query drops: they match nearly every chunk (English; other languages are a later decision). */
static const char *const STOPWORDS[] = {
    "a",    "about", "an",    "and",   "are",   "as",    "at",   "be",   "but",   "by",     "can", "could", "did",
    "do",   "does",  "for",   "from",  "had",   "has",   "have", "how",  "i",     "if",     "in",  "into",  "is",
    "it",   "its",   "me",    "my",    "no",    "not",   "of",   "on",   "or",    "should", "so",  "than",  "that",
    "the",  "their", "them",  "then",  "there", "these", "they", "this", "those", "to",     "was", "we",    "were",
    "what", "when",  "where", "which", "who",   "whom",  "why",  "will", "with",  "would",  "you", "your",
};

typedef struct {
    arcpack *pack;
    sqlite3 *db;
    sqlite3_stmt *query;
} pack_index;

struct arcsearch {
    pack_index packs[ARCSEARCH_MAX_PACKS];
    size_t n_packs;
    char warnings[ARCSEARCH_MAX_WARNINGS][320];
    size_t n_warnings;
    char err[320];
};

static _Thread_local char open_why[320];

/* warn: record one arcsearch_open() warning (later ones past the limit are dropped). */
__attribute__((format(printf, 2, 3))) static void warn(arcsearch *s, const char *fmt, ...) {
    if (s->n_warnings == ARCSEARCH_MAX_WARNINGS) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->warnings[s->n_warnings++], sizeof(s->warnings[0]), fmt, ap);
    va_end(ap);
}

/* search_fail: set the error of `s`, point *why at it, return false. */
__attribute__((format(printf, 3, 4))) static bool search_fail(arcsearch *s, const char **why, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof(s->err), fmt, ap);
    va_end(ap);
    *why = s->err;
    return false;
}

/* ---- Finding packs ------------------------------------------------------------------------------ */

/* is_dir: `parent`/`name` is a directory. */
static bool is_dir(const char *parent, const char *name) {
    char path[PATH_MAX];
    struct stat st;
    return snprintf(path, sizeof(path), "%s/%s", parent, name) < (int)sizeof(path) && stat(path, &st) == 0 &&
           S_ISDIR(st.st_mode);
}

/* highest_version: the highest semver-named directory in `dir`, into out; false if there is none. */
static bool highest_version(const char *dir, char out[ARCPACK_NAME_MAX + 1]) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return false;
    }
    out[0] = '\0';
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        size_t len = strlen(e->d_name);
        if (arcjson_is_schema_version(e->d_name, len) && is_dir(dir, e->d_name) &&
            (out[0] == '\0' || arcjson_schema_version_cmp(e->d_name, out) > 0)) {
            memcpy(out, e->d_name, len + 1);
        }
    }
    closedir(d);
    return out[0] != '\0';
}

/* uri_path: "file:<path, with % ? # escaped>?immutable=1" for sqlite3_open_v2 (SQLITE_OPEN_URI). */
static bool uri_path(const char *path, char *buf, size_t cap) {
    size_t n = (size_t)snprintf(buf, cap, "file:");
    for (const char *c = path; *c != '\0'; c++) {
        if (n + 4 >= cap) {
            return false;
        }
        if (*c == '%' || *c == '?' || *c == '#') {
            n += (size_t)snprintf(buf + n, cap - n, "%%%02X", (unsigned char)*c);
        } else {
            buf[n++] = *c;
        }
    }
    return (size_t)snprintf(buf + n, cap - n, "?immutable=1") < cap - n;
}

/* meta_matches: the index's meta rows describe this pack; else *stale says how they differ. */
static bool meta_matches(sqlite3 *db, const arcpack *p, char *stale, size_t cap) {
    char want_count[16];
    snprintf(want_count, sizeof(want_count), "%u", arcpack_chunk_count(p));
    const char *keys[] = {"schema_version", "pack_name", "pack_version", "chunk_count"};
    const char *want[] = {"1", arcpack_name(p), arcpack_version(p), want_count};
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?1", -1, &st, NULL) != SQLITE_OK) {
        snprintf(stale, cap, "no meta table (%s)", sqlite3_errmsg(db));
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(keys) / sizeof(keys[0]); i++) {
        sqlite3_bind_text(st, 1, keys[i], -1, SQLITE_STATIC);
        const unsigned char *got = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_text(st, 0) : NULL;
        if (got == NULL || strcmp((const char *)got, want[i]) != 0) {
            snprintf(stale, cap, "%s is %s, the pack has %s", keys[i], got ? (const char *)got : "missing", want[i]);
            ok = false;
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return ok;
}

/* close_index: close one pack's index and pack. */
static void close_index(pack_index *ix) {
    sqlite3_finalize(ix->query);
    sqlite3_close(ix->db);
    arcpack_close(ix->pack);
}

/* add_pack: open pack `name` at `dir` with its index; a pack that fails is skipped with a warning. */
static void add_pack(arcsearch *s, const char *name, const char *version, const char *dir) {
    const char *err;
    pack_index ix = {0};
    ix.pack = arcpack_open(dir, &err);
    if (ix.pack == NULL) {
        warn(s, "pack %s %s skipped: %s", name, version, err);
        return;
    }
    if (strcmp(arcpack_name(ix.pack), name) != 0 || strcmp(arcpack_version(ix.pack), version) != 0) {
        warn(s, "pack %s %s skipped: its manifest says %s %s", name, version, arcpack_name(ix.pack),
             arcpack_version(ix.pack));
        close_index(&ix);
        return;
    }
    char path[PATH_MAX];
    char uri[PATH_MAX + 32];
    char stale[160];
    if (snprintf(path, sizeof(path), "%s/fts/index.sqlite", dir) >= (int)sizeof(path) ||
        !uri_path(path, uri, sizeof(uri))) {
        warn(s, "pack %s %s skipped: path too long", name, version);
    } else if (sqlite3_open_v2(uri, &ix.db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
        warn(s, "pack %s %s skipped: fts/index.sqlite: %s", name, version, sqlite3_errmsg(ix.db));
    } else if (!meta_matches(ix.db, ix.pack, stale, sizeof(stale))) {
        warn(s, "pack %s %s skipped: stale index (%s)", name, version, stale);
    } else if (sqlite3_prepare_v3(ix.db, QUERY, -1, SQLITE_PREPARE_PERSISTENT, &ix.query, NULL) != SQLITE_OK) {
        warn(s, "pack %s %s skipped: fts/index.sqlite: %s", name, version, sqlite3_errmsg(ix.db));
    } else {
        s->packs[s->n_packs++] = ix;
        return;
    }
    close_index(&ix);
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* list_packs: the pack-name directories in `packs`, sorted (so results are in a fixed pack order). */
static size_t list_packs(arcsearch *s, DIR *d, const char *packs, char *names[ARCSEARCH_MAX_PACKS]) {
    size_t n = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (!arcjson_is_schema_id(e->d_name, strlen(e->d_name)) || !is_dir(packs, e->d_name)) {
            continue;
        }
        if (n == ARCSEARCH_MAX_PACKS) {
            warn(s, "more than %u packs: %s and later ones skipped", ARCSEARCH_MAX_PACKS, e->d_name);
            break;
        }
        names[n] = strdup(e->d_name);
        n += names[n] != NULL;
    }
    qsort(names, n, sizeof(names[0]), name_cmp);
    return n;
}

arcsearch *arcsearch_open(const char *root, const char **why) {
    char packs[PATH_MAX];
    if (snprintf(packs, sizeof(packs), "%s/packs", root) >= (int)sizeof(packs)) {
        *why = "root path too long";
        return NULL;
    }
    arcsearch *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        *why = "out of memory";
        return NULL;
    }
    DIR *d = opendir(packs);
    if (d == NULL) {
        if (errno == ENOENT) { /* no packs installed: searchable, with nothing to find */
            warn(s, "%s: no packs installed", packs);
            return s;
        }
        snprintf(open_why, sizeof(open_why), "%.200s: %.100s", packs, strerror(errno));
        *why = open_why;
        free(s);
        return NULL;
    }
    char *names[ARCSEARCH_MAX_PACKS];
    size_t n = list_packs(s, d, packs, names);
    closedir(d);
    for (size_t i = 0; i < n; i++) {
        char dir[PATH_MAX];
        char version[ARCPACK_NAME_MAX + 1];
        if (snprintf(dir, sizeof(dir), "%s/%s", packs, names[i]) >= (int)sizeof(dir) ||
            !highest_version(dir, version)) {
            warn(s, "pack %s skipped: no installed version", names[i]);
        } else if (snprintf(dir, sizeof(dir), "%s/%s/%s", packs, names[i], version) < (int)sizeof(dir)) {
            add_pack(s, names[i], version, dir);
        }
        free(names[i]);
    }
    return s;
}

void arcsearch_close(arcsearch *s) {
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->n_packs; i++) {
        close_index(&s->packs[i]);
    }
    free(s);
}

size_t arcsearch_pack_count(const arcsearch *s) {
    return s->n_packs;
}

const char *arcsearch_pack_name(const arcsearch *s, size_t i) {
    return i < s->n_packs ? arcpack_name(s->packs[i].pack) : NULL;
}

size_t arcsearch_warning_count(const arcsearch *s) {
    return s->n_warnings;
}

const char *arcsearch_warning(const arcsearch *s, size_t i) {
    return i < s->n_warnings ? s->warnings[i] : NULL;
}

/* ---- Queries ------------------------------------------------------------------------------------ */

/* word_char: a Unicode letter or digit, or a combining accent (U+0300-U+036F) that belongs to the letter before it. */
static bool word_char(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
    }
    return iswalnum((wint_t)cp) || (cp >= 0x300 && cp <= 0x36F);
}

static bool is_stopword(const char *w, size_t len) {
    for (size_t i = 0; i < sizeof(STOPWORDS) / sizeof(STOPWORDS[0]); i++) {
        if (strlen(STOPWORDS[i]) == len && memcmp(STOPWORDS[i], w, len) == 0) {
            return true;
        }
    }
    return false;
}

size_t arcsearch_match_expr(const char *query, char *buf, size_t cap) {
    size_t n = 0;
    size_t qlen = strlen(query);
    if (cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < qlen;) {
        uint32_t cp;
        size_t len = arcjson_utf8_decode(query + i, qlen - i, &cp);
        if (len == 0 || !word_char(cp)) { /* separators, and bytes that are not UTF-8 */
            i += len ? len : 1;
            continue;
        }
        size_t start = i;
        while (i < qlen && (len = arcjson_utf8_decode(query + i, qlen - i, &cp)) != 0 && word_char(cp)) {
            i += len;
        }
        size_t wlen = i - start;
        size_t need = (n ? 4 : 0) + wlen + 2; /* " OR " + quoted word */
        if (n + need + 1 > cap) {
            break;
        }
        size_t at = n + (n ? 4 : 0);
        memcpy(buf + at + 1, query + start, wlen);
        for (size_t j = 0; j < wlen; j++) { /* ASCII lowercase; FTS5 folds the rest */
            char c = buf[at + 1 + j];
            buf[at + 1 + j] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        if (is_stopword(buf + at + 1, wlen)) {
            continue;
        }
        if (n) {
            memcpy(buf + n, " OR ", 4);
        }
        buf[at] = '"';
        buf[at + 1 + wlen] = '"';
        n = at + wlen + 2;
    }
    buf[n] = '\0';
    return n;
}

/* keep: put a hit into results[0..*n) (best first, at most k), if it is among the best k so far. */
static bool keep(arcsearch *s, pack_index *ix, uint32_t chunk, double score, arcsearch_result *results, size_t k,
                 size_t *n, const char **why) {
    if (*n == k && score <= results[k - 1].score) {
        return true;
    }
    arcpack_chunk c;
    arcpack_doc doc;
    if (!arcpack_chunk_get(ix->pack, chunk, &c) || !arcpack_doc_get(ix->pack, c.document, &doc)) {
        return search_fail(s, why, "pack %s: %s", arcpack_name(ix->pack), arcpack_error(ix->pack));
    }
    size_t at = *n < k ? (*n)++ : k - 1;
    while (at > 0 && results[at - 1].score < score) { /* equal scores keep pack, then rank, order */
        results[at] = results[at - 1];
        at--;
    }
    arcsearch_result *r = &results[at];
    arcpack_chunk_id(ix->pack, chunk, r->id, sizeof(r->id));
    r->score = score;
    r->chunk = chunk;
    r->pack = arcpack_name(ix->pack);
    r->title = doc.title;
    r->title_length = doc.title_length;
    r->section = c.section;
    r->section_length = c.section_length;
    return true;
}

/* search_pack: one pack's best k for expr, merged into results. */
static bool search_pack(arcsearch *s, pack_index *ix, const char *expr, arcsearch_result *results, size_t k, size_t *n,
                        const char **why) {
    sqlite3_stmt *q = ix->query;
    bool ok = sqlite3_bind_text(q, 1, expr, -1, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int64(q, 2, (sqlite3_int64)k) == SQLITE_OK;
    int rc = SQLITE_DONE;
    while (ok && (rc = sqlite3_step(q)) == SQLITE_ROW) {
        sqlite3_int64 rowid = sqlite3_column_int64(q, 0);
        if (rowid < 0 || rowid >= arcpack_chunk_count(ix->pack)) {
            ok = search_fail(s, why, "pack %s: index row %lld is not a chunk", arcpack_name(ix->pack), rowid);
            break;
        }
        ok = keep(s, ix, (uint32_t)rowid, -sqlite3_column_double(q, 1), results, k, n, why);
    }
    if (ok && rc != SQLITE_DONE) {
        ok = search_fail(s, why, "pack %s: %s", arcpack_name(ix->pack), sqlite3_errmsg(ix->db));
    }
    sqlite3_reset(q);
    sqlite3_clear_bindings(q);
    return ok;
}

bool arcsearch_search(arcsearch *s, const char *query, const char *pack, size_t k, arcsearch_result *results, size_t *n,
                      const char **why) {
    char expr[EXPR_MAX];
    *n = 0;
    bool found = pack == NULL;
    for (size_t i = 0; !found && i < s->n_packs; i++) {
        found = strcmp(arcpack_name(s->packs[i].pack), pack) == 0;
    }
    if (!found) {
        return search_fail(s, why, "no installed pack named '%s'", pack);
    }
    if (k == 0 || arcsearch_match_expr(query, expr, sizeof(expr)) == 0) {
        return true;
    }
    for (size_t i = 0; i < s->n_packs; i++) {
        if ((pack == NULL || strcmp(arcpack_name(s->packs[i].pack), pack) == 0) &&
            !search_pack(s, &s->packs[i], expr, results, k, n, why)) {
            return false;
        }
    }
    return true;
}
