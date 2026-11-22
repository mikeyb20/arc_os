/*
 * build.c — reading an article stream and writing the pack (packbuild.h).
 *
 * Pass 1 checks the header and collects the keys in one buffer (they arrive sorted, so the array
 * is sorted as read and a repeat or a step back is an error). Pass 2 reads each document again,
 * rewrites its links, chunks it and hands it to the arcpack writer.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arcjson.h"
#include "arcpack.h"
#include "arcutil.h"
#include "packbuild.h"

#define WHY_MAX 512

static _Thread_local char build_why[WHY_MAX];

/* Everything one build holds. */
typedef struct {
    FILE *f;
    const char *path;
    char *line; /* current line, NUL-terminated (newline removed) */
    size_t line_len, line_cap;
    uint64_t line_no; /* 1-based */
    arcjson_arena arena;
    char *keys; /* all keys back to back */
    size_t keys_len, keys_cap;
    uint64_t *key_off; /* key i is keys[key_off[i] .. key_off[i + 1]) */
    size_t n_keys, key_off_cap;
    char *text; /* the current document after link rewriting */
    size_t text_len, text_cap;
    packbuild_chunks chunks;
    arcpack_chunk_spec *specs;
    size_t specs_cap;
} build;

/* pb_fail: *why = the formatted message (static buffer); false. */
__attribute__((format(printf, 2, 3))) static bool pb_fail(const char **why, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(build_why, sizeof(build_why), fmt, ap);
    va_end(ap);
    *why = build_why;
    return false;
}

/* reserve: room for `need` bytes in *buf (capacity *cap). */
static bool reserve(char **buf, size_t *cap, size_t need) {
    if (need <= *cap) {
        return true;
    }
    size_t c = *cap ? *cap : 4096;
    while (c < need) {
        c *= 2;
    }
    char *p = realloc(*buf, c);
    if (p == NULL) {
        return false;
    }
    *buf = p;
    *cap = c;
    return true;
}

/*
 * read_stream_line: the next line of the stream into b->line (newline removed). 1 = a line, 0 = end of
 * file, -1 = error (*why set): a line over PACKBUILD_LINE_MAX bytes, or a read error.
 */
static int read_stream_line(build *b, const char **why) {
    b->line_len = 0;
    int c;
    while ((c = getc(b->f)) != EOF && c != '\n') {
        if (b->line_len == PACKBUILD_LINE_MAX) {
            pb_fail(why, "%s: line %llu is longer than %u bytes", b->path, (unsigned long long)b->line_no + 1,
                    PACKBUILD_LINE_MAX);
            return -1;
        }
        if (b->line_len + 2 > b->line_cap && !reserve(&b->line, &b->line_cap, b->line_len + 2)) {
            pb_fail(why, "out of memory");
            return -1;
        }
        b->line[b->line_len++] = (char)c;
    }
    if (ferror(b->f)) {
        pb_fail(why, "%s: read error: %s", b->path, strerror(errno));
        return -1;
    }
    if (c == EOF && b->line_len == 0) {
        return 0;
    }
    if (c == EOF) {
        pb_fail(why, "%s: line %llu has no newline at its end (file cut short?)", b->path,
                (unsigned long long)b->line_no + 1);
        return -1;
    }
    if (b->line == NULL && !reserve(&b->line, &b->line_cap, 1)) {
        pb_fail(why, "out of memory");
        return -1;
    }
    b->line[b->line_len] = '\0';
    b->line_no++;
    return 1;
}

/* parse_doc_line: the current line as a JSON object. */
static const arcjson_value *parse_doc_line(build *b, const char **why) {
    arcjson_arena_free(&b->arena);
    arcjson_error err;
    const arcjson_value *v = arcjson_parse(&b->arena, b->line, b->line_len, PACKBUILD_LINE_MAX, &err);
    if (v == NULL || v->type != ARCJSON_OBJECT) {
        pb_fail(why, "%s: line %llu: %s", b->path, (unsigned long long)b->line_no,
                v == NULL ? err.message : "not a JSON object");
        return NULL;
    }
    return v;
}

/* string_member: member `key` of `obj` as a string (may hold NUL bytes: len is the length). */
static bool string_member(build *b, const arcjson_value *obj, const char *key, const char **s, size_t *len,
                          const char **why) {
    const arcjson_value *v = arcjson_get(obj, key);
    if (v == NULL || v->type != ARCJSON_STRING) {
        return pb_fail(why, "%s: line %llu: no string \"%s\"", b->path, (unsigned long long)b->line_no, key);
    }
    *s = v->u.string.ptr;
    *len = v->u.string.len;
    return true;
}

/* read_stream_header: line 1, {"stream": 1, ...}. */
static bool read_stream_header(build *b, const char **why) {
    int r = read_stream_line(b, why);
    if (r <= 0) {
        return r == 0 ? pb_fail(why, "%s: empty stream (no header line)", b->path) : false;
    }
    const arcjson_value *h = parse_doc_line(b, why);
    int64_t version;
    if (h == NULL) {
        return false;
    }
    if (!arcjson_int64(arcjson_get(h, "stream"), &version) || version != 1) {
        return pb_fail(why, "%s: line 1 is not an article stream version 1 header", b->path);
    }
    return true;
}

/* stream_key_cmp: memcmp order of two keys, the shorter first when one is a prefix of the other. */
static int stream_key_cmp(const char *a, size_t al, const char *b, size_t bl) {
    int c = memcmp(a, b, al < bl ? al : bl);
    return c != 0 ? c : (al > bl) - (al < bl);
}

/* pass1: every document's key, in order. */
static bool pass1(build *b, const char **why) {
    if (!read_stream_header(b, why)) {
        return false;
    }
    int r;
    while ((r = read_stream_line(b, why)) == 1) {
        const arcjson_value *doc = parse_doc_line(b, why);
        const char *key;
        size_t len;
        if (doc == NULL || !string_member(b, doc, "key", &key, &len, why)) {
            return false;
        }
        if (len == 0 || memchr(key, '\0', len) != NULL) {
            return pb_fail(why, "%s: line %llu: empty key or key with a NUL", b->path, (unsigned long long)b->line_no);
        }
        if (b->n_keys > 0) {
            uint64_t prev = b->key_off[b->n_keys - 1];
            if (stream_key_cmp(b->keys + prev, b->keys_len - prev, key, len) >= 0) {
                return pb_fail(why, "%s: line %llu: key is not after the previous one (repeated or out of order)",
                               b->path, (unsigned long long)b->line_no);
            }
        }
        if (b->n_keys + 2 > b->key_off_cap) {
            size_t c = b->key_off_cap ? 2 * b->key_off_cap : 1024;
            uint64_t *p = realloc(b->key_off, c * sizeof(*p));
            if (p == NULL) {
                return pb_fail(why, "out of memory");
            }
            b->key_off = p;
            b->key_off_cap = c;
        }
        if (!reserve(&b->keys, &b->keys_cap, b->keys_len + len)) {
            return pb_fail(why, "out of memory");
        }
        if (b->n_keys == UINT32_MAX) {
            return pb_fail(why, "%s: more documents than pack-v1 allows", b->path);
        }
        b->key_off[b->n_keys++] = b->keys_len;
        memcpy(b->keys + b->keys_len, key, len);
        b->keys_len += len;
        b->key_off[b->n_keys] = b->keys_len;
    }
    return r == 0;
}

/* find_key: the document ID of key s[0..len), or -1. O(log documents). */
static int64_t find_key(const build *b, const char *s, size_t len) {
    size_t lo = 0;
    size_t hi = b->n_keys;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = stream_key_cmp(b->keys + b->key_off[mid], b->key_off[mid + 1] - b->key_off[mid], s, len);
        if (c == 0) {
            return (int64_t)mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return -1;
}

static bool append(build *b, const char *s, size_t n) {
    if (!reserve(&b->text, &b->text_cap, b->text_len + n + 1)) {
        return false;
    }
    memcpy(b->text + b->text_len, s, n);
    b->text_len += n;
    return true;
}

/*
 * rewrite_links: text[0..len) into b->text with every link's key replaced by its document ID, or the
 * link replaced by its text when the key is not in the stream. A link must close on its own line.
 */
static bool rewrite_links(build *b, const char *s, size_t len, packbuild_stats *st, const char **why) {
    b->text_len = 0;
    size_t run = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] != ARCPACK_MARK_LINK) {
            continue;
        }
        size_t sep = i + 1;
        while (sep < len && s[sep] != ARCPACK_MARK_SEP && s[sep] != '\n' && s[sep] != ARCPACK_MARK_LINK_END) {
            sep++;
        }
        size_t close = sep + 1;
        while (close < len && s[close] != ARCPACK_MARK_LINK_END && s[close] != '\n') {
            close++;
        }
        if (sep >= len || s[sep] != ARCPACK_MARK_SEP || close >= len || s[close] != ARCPACK_MARK_LINK_END ||
            sep == i + 1) {
            return pb_fail(why, "%s: line %llu: malformed link at byte %zu of the text", b->path,
                           (unsigned long long)b->line_no, i);
        }
        if (!append(b, s + run, i - run)) {
            return pb_fail(why, "out of memory");
        }
        int64_t id = find_key(b, s + i + 1, sep - i - 1);
        if (id >= 0) {
            char num[24];
            int n = snprintf(num, sizeof(num), "%c%lld%c", ARCPACK_MARK_LINK, (long long)id, ARCPACK_MARK_SEP);
            if (!append(b, num, (size_t)n) || !append(b, s + sep + 1, close + 1 - (sep + 1))) {
                return pb_fail(why, "out of memory");
            }
            st->links++;
        } else {
            if (!append(b, s + sep + 1, close - (sep + 1))) {
                return pb_fail(why, "out of memory");
            }
            st->links_dropped++;
        }
        run = close + 1;
        i = close;
    }
    if (!append(b, s + run, len - run)) {
        return pb_fail(why, "out of memory");
    }
    return true;
}

/* add_document: the current line (a document) into the pack. */
static bool add_document(build *b, arcpack_writer *w, const packbuild_options *opt, uint64_t tags, packbuild_stats *st,
                         const char **why) {
    const arcjson_value *doc = parse_doc_line(b, why);
    const char *key, *title, *text;
    size_t key_len, title_len, text_len;
    if (doc == NULL || !string_member(b, doc, "key", &key, &key_len, why) ||
        !string_member(b, doc, "title", &title, &title_len, why) ||
        !string_member(b, doc, "text", &text, &text_len, why)) {
        return false;
    }
    uint32_t id = st->documents;
    if (id >= b->n_keys ||
        stream_key_cmp(b->keys + b->key_off[id], b->key_off[id + 1] - b->key_off[id], key, key_len)) {
        return pb_fail(why, "%s: line %llu: the stream changed between the two passes", b->path,
                       (unsigned long long)b->line_no);
    }
    if (!rewrite_links(b, text, text_len, st, why)) {
        return false;
    }
    if (b->text_len > UINT32_MAX) {
        return pb_fail(why, "%s: line %llu: document text larger than pack-v1 allows", b->path,
                       (unsigned long long)b->line_no);
    }
    if (!packbuild_chunk_text(b->text, b->text_len, &opt->chunking, &b->chunks)) {
        return pb_fail(why, "out of memory");
    }
    if (b->chunks.n > b->specs_cap) {
        arcpack_chunk_spec *p = realloc(b->specs, b->chunks.n * sizeof(*p));
        if (p == NULL) {
            return pb_fail(why, "out of memory");
        }
        b->specs = p;
        b->specs_cap = b->chunks.n;
    }
    for (size_t i = 0; i < b->chunks.n; i++) {
        const packbuild_chunk *c = &b->chunks.chunk[i];
        b->specs[i] =
            (arcpack_chunk_spec){c->offset, c->length, b->chunks.paths + c->section_offset, c->section_length, tags};
        st->words_hist[c->words > opt->chunking.max ? PACKBUILD_WORDS_HIST - 1 : c->words]++;
        st->words_max = c->words > st->words_max ? c->words : st->words_max;
    }
    const char *wwhy;
    if (!arcpack_writer_add_doc(w, title, title_len, b->text, b->text_len, b->specs, b->chunks.n, &wwhy)) {
        return pb_fail(why, "%s: line %llu (key \"%.*s\"): %s", b->path, (unsigned long long)b->line_no,
                       (int)(key_len > 200 ? 200 : key_len), key, wwhy);
    }
    st->documents++;
    st->chunks += (uint32_t)b->chunks.n;
    st->text_bytes += b->text_len;
    st->docs_no_chunks += b->chunks.n == 0;
    return true;
}

/* pass2: every document into the writer. */
static bool pass2(build *b, arcpack_writer *w, const packbuild_options *opt, packbuild_stats *st, const char **why) {
    if (fseek(b->f, 0, SEEK_SET) != 0) {
        return pb_fail(why, "%s: cannot read it twice (not a regular file?): %s", b->path, strerror(errno));
    }
    b->line_no = 0;
    if (!read_stream_header(b, why)) {
        return false;
    }
    uint64_t tags = opt->n_tags == 64 ? UINT64_MAX : (UINT64_C(1) << opt->n_tags) - 1;
    int r;
    while ((r = read_stream_line(b, why)) == 1) {
        if (!add_document(b, w, opt, tags, st, why)) {
            return false;
        }
    }
    if (r == 0 && st->documents != b->n_keys) {
        return pb_fail(why, "%s: the stream changed between the two passes", b->path);
    }
    return r == 0;
}

/* check_options: tags are manifest ids, unique, at most 64; chunking sane. */
static bool check_options(const packbuild_options *opt, const char **why) {
    if (opt->n_tags > ARCPACK_MAX_TAGS) {
        return pb_fail(why, "more than %u domain tags", ARCPACK_MAX_TAGS);
    }
    for (size_t i = 0; i < opt->n_tags; i++) {
        if (!arcjson_is_schema_id(opt->tags[i], strlen(opt->tags[i]))) {
            return pb_fail(why, "domain tag \"%s\" is not a manifest id (a-z, 0-9, '-')", opt->tags[i]);
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(opt->tags[i], opt->tags[j]) == 0) {
                return pb_fail(why, "domain tag \"%s\" given twice", opt->tags[i]);
            }
        }
    }
    const packbuild_chunking *c = &opt->chunking;
    if (c->target < 4 || c->max < c->target || c->overlap >= c->target - c->target / 4) {
        return pb_fail(why, "chunking: need target >= 4, max >= target, overlap < 3/4 of target");
    }
    return true;
}

/* empty_dir: `dir` exists and holds nothing (true), or does not exist (*exists false). */
static bool usable_outdir(const char *dir, bool *exists, const char **why) {
    struct stat sb;
    if (stat(dir, &sb) != 0) {
        if (errno != ENOENT) {
            return pb_fail(why, "%s: %s", dir, strerror(errno));
        }
        *exists = false;
        return true;
    }
    *exists = true;
    if (!S_ISDIR(sb.st_mode) || rmdir(dir) != 0) { /* rmdir succeeds only on an empty directory */
        return pb_fail(why, "%s: exists and is not an empty directory", dir);
    }
    return true;
}

bool packbuild_build(const char *stream, const char *outdir, const packbuild_options *opt, packbuild_stats *st,
                     const char **why) {
    memset(st, 0, sizeof(*st));
    bool existed;
    if (!check_options(opt, why) || !usable_outdir(outdir, &existed, why)) {
        return false;
    }
    build b = {.path = stream};
    arcjson_arena_init(&b.arena);
    arcpack_writer *w = NULL;
    bool ok = (b.f = fopen(stream, "rbe")) != NULL || pb_fail(why, "%s: %s", stream, strerror(errno));
    ok = ok && pass1(&b, why);
    const char *wwhy = NULL;
    if (ok && (w = arcpack_writer_open(outdir, opt->name, opt->version, &wwhy)) == NULL) {
        ok = pb_fail(why, "%s", wwhy);
    }
    ok = ok && pass2(&b, w, opt, st, why);
    if (ok) {
        ok = arcpack_writer_finish(w, &wwhy) || pb_fail(why, "%s", wwhy);
        w = NULL;
    }
    arcpack_writer_abort(w);
    if (!ok) {
        arcutil_remove_tree(outdir); /* nothing left behind; an empty directory given to us stays */
        if (existed) {
            mkdir(outdir, 0755);
        }
    }
    if (b.f != NULL) {
        fclose(b.f);
    }
    arcjson_arena_free(&b.arena);
    packbuild_chunks_free(&b.chunks);
    free(b.line);
    free(b.keys);
    free(b.key_off);
    free(b.text);
    free(b.specs);
    return ok;
}

uint32_t packbuild_words_median(const packbuild_stats *st) {
    if (st->chunks == 0) {
        return 0;
    }
    uint64_t half = ((uint64_t)st->chunks + 1) / 2;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < PACKBUILD_WORDS_HIST; i++) {
        seen += st->words_hist[i];
        if (seen >= half) {
            return i;
        }
    }
    return PACKBUILD_WORDS_HIST - 1;
}
