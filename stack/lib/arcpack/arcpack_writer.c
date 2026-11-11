/*
 * arcpack_writer.c — the pack writer (see arcpack.h; format in docs/formats/pack-v1.md).
 *
 * Each document is checked first (UTF-8, markup, chunk ranges, limits), then appended to the four
 * files: its zstd frame to content/docs.zst, its title and new section paths to content/strings,
 * one record to content/docs.idx and one per chunk to chunks/chunks.idx. The files are written
 * under ".part" names behind a zeroed header; finish() fills the headers, fsyncs and renames them.
 *
 * Memory does not grow with the pack: four write buffers, the zstd context, the last section
 * path and the markup spans of the current document.
 */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

#include "arcutil.h"
#include "arcjson.h"
#include "arcpack_internal.h"

#define WRITE_BUF (256u * 1024u)

/* A markup construct [start, end): a chunk may not start or end strictly inside it. */
typedef struct {
    size_t start;
    size_t end;
} span;

struct arcpack_writer {
    char *dir;
    FILE *f[ARCPACK_NFILES];
    char *fbuf[ARCPACK_NFILES];
    uint64_t zst_pos;     /* bytes of frames written after the docs.zst header */
    uint64_t strings_len; /* data bytes of content/strings */
    uint32_t n_docs;
    uint32_t n_chunks;
    ZSTD_CCtx *cctx;
    void *zbuf;
    size_t zbuf_cap;
    char name[ARCPACK_NAME_MAX];    /* NUL-padded, as in the headers */
    char version[ARCPACK_NAME_MAX]; /* NUL-padded */
    char *last_section;             /* last section path written, shared by the next chunk if equal */
    size_t last_section_len;
    size_t last_section_cap;
    uint32_t last_section_off;
    bool have_last_section;
    bool any_link;
    uint32_t max_link;     /* highest document ID a link of an added document names; checked at finish */
    bool doc_any_link;     /* the same for the document being checked, kept only if it is added */
    uint32_t doc_max_link;
    span *spans;
    size_t n_spans;
    size_t spans_cap;
    bool failed; /* an I/O error: the writer can only be aborted */
    char err[256];
};

/* writer_fail: format the writer's error, point *why at it and return false. */
__attribute__((format(printf, 3, 4))) static bool writer_fail(arcpack_writer *w, const char **why, const char *fmt,
                                                              ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->err, sizeof(w->err), fmt, ap);
    va_end(ap);
    *why = w->err;
    return false;
}

/* path_of: dir/rel[suffix] into buf. */
static bool path_of(const arcpack_writer *w, const char *rel, const char *suffix, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "%s/%s%s", w->dir, rel, suffix);
    return n >= 0 && (size_t)n < cap;
}

/* ---- Opening ------------------------------------------------------------------------------------ */

/* close_files: close and free every open file; with `discard`, remove the .part files. */
static void close_files(arcpack_writer *w, bool discard) {
    for (int f = 0; f < ARCPACK_NFILES; f++) {
        char path[PATH_MAX];
        if (w->f[f] != NULL) {
            fclose(w->f[f]);
            w->f[f] = NULL;
        }
        if (discard && path_of(w, arcpack_files[f].path, ".part", path, sizeof(path))) {
            unlink(path); /* also a .part already sealed by a finish() that failed later */
        }
        free(w->fbuf[f]);
        w->fbuf[f] = NULL;
    }
}

static void writer_free(arcpack_writer *w) {
    ZSTD_freeCCtx(w->cctx);
    free(w->zbuf);
    free(w->last_section);
    free(w->spans);
    free(w->dir);
    free(w);
}

/* open_files: create the directories and the four .part files, each starting with a zeroed header. */
static bool open_files(arcpack_writer *w, const char **why) {
    char path[PATH_MAX];
    static const char *const dirs[] = {"", "/content", "/chunks"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (snprintf(path, sizeof(path), "%s%s", w->dir, dirs[i]) >= (int)sizeof(path) || !arcutil_make_dir(path)) {
            return writer_fail(w, why, "%s%s: cannot create directory: %s", w->dir, dirs[i], strerror(errno));
        }
    }
    static const char zero[ARCPACK_HEADER_SIZE];
    for (int f = 0; f < ARCPACK_NFILES; f++) {
        if (!path_of(w, arcpack_files[f].path, ".part", path, sizeof(path))) {
            return writer_fail(w, why, "%s: path too long", w->dir);
        }
        w->f[f] = fopen(path, "wbe");
        w->fbuf[f] = malloc(WRITE_BUF);
        if (w->f[f] == NULL || w->fbuf[f] == NULL) {
            return writer_fail(w, why, "%s: %s", path, w->f[f] == NULL ? strerror(errno) : "out of memory");
        }
        setvbuf(w->f[f], w->fbuf[f], _IOFBF, WRITE_BUF);
        if (fwrite(zero, 1, sizeof(zero), w->f[f]) != sizeof(zero)) {
            return writer_fail(w, why, "%s: write failed", path);
        }
    }
    return true;
}

/* zstd_setup: level 19, window at most 8 MiB, checksum and content size in every frame (plain frames). */
static bool zstd_setup(arcpack_writer *w, const char **why) {
    w->cctx = ZSTD_createCCtx();
    w->zbuf_cap = ZSTD_CStreamOutSize();
    w->zbuf = malloc(w->zbuf_cap);
    if (w->cctx == NULL || w->zbuf == NULL ||
        ZSTD_isError(ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_compressionLevel, ARCPACK_ZSTD_LEVEL)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_windowLog, ARCPACK_ZSTD_WINDOW_LOG)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_checksumFlag, 1)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_contentSizeFlag, 1))) {
        return writer_fail(w, why, "zstd setup failed");
    }
    return true;
}

arcpack_writer *arcpack_writer_open(const char *dir, const char *name, const char *version, const char **why) {
    static _Thread_local char open_why[256];
    arcpack_writer *w = calloc(1, sizeof(*w));
    if (w == NULL || (w->dir = strdup(dir)) == NULL) {
        free(w);
        *why = "out of memory";
        return NULL;
    }
    bool ok;
    if (!arcjson_is_schema_id(name, strlen(name))) {
        ok = writer_fail(w, why, "pack name '%s' is not a manifest id (a-z, 0-9, '-')", name);
    } else if (!arcjson_is_schema_version(version, strlen(version))) {
        ok = writer_fail(w, why, "pack version '%s' is not 1-64 semver characters starting with a digit", version);
    } else {
        memcpy(w->name, name, strlen(name));
        memcpy(w->version, version, strlen(version));
        ok = zstd_setup(w, why) && open_files(w, why);
    }
    if (!ok) {
        snprintf(open_why, sizeof(open_why), "%s", w->err);
        *why = open_why;
        arcpack_writer_abort(w);
        return NULL;
    }
    return w;
}

/* ---- Checking a document ------------------------------------------------------------------------ */

static bool is_control(unsigned char c) {
    return c < 0x20 || c == 0x7F;
}

/* plain_run: s[a, b) holds no control characters (`tab_ok`: except tab). */
static bool plain_run(const char *s, size_t a, size_t b, bool tab_ok) {
    for (size_t i = a; i < b; i++) {
        if (is_control((unsigned char)s[i]) && !(tab_ok && s[i] == '\t')) {
            return false;
        }
    }
    return true;
}

static bool add_span(arcpack_writer *w, size_t start, size_t end) {
    if (w->n_spans == w->spans_cap) {
        size_t cap = w->spans_cap ? 2 * w->spans_cap : 64;
        span *s = realloc(w->spans, cap * sizeof(*s));
        if (s == NULL) {
            return false;
        }
        w->spans = s;
        w->spans_cap = cap;
    }
    w->spans[w->n_spans++] = (span){start, end};
    return true;
}

/* find_sep: index of the first separator in s[a, b), or b. */
static size_t find_sep(const char *s, size_t a, size_t b) {
    const char *sep = memchr(s + a, ARCPACK_MARK_SEP, b - a);
    return sep ? (size_t)(sep - s) : b;
}

/*
 * check_link: the link starting at s[a] (the link byte) inside line end b. Sets *end past its
 * closing byte and records the target. Format: 02 target 1F text 03.
 */
static bool check_link(arcpack_writer *w, const char *s, size_t a, size_t b, size_t *end, const char **why) {
    size_t sep = find_sep(s, a + 1, b);
    const char *close = sep < b ? memchr(s + sep, ARCPACK_MARK_LINK_END, b - sep) : NULL;
    size_t digits = sep - a - 1;
    uint64_t target = 0;
    bool ok = close != NULL && digits >= 1 && digits <= 10 && !(digits > 1 && s[a + 1] == '0');
    for (size_t i = a + 1; ok && i < sep; i++) {
        ok = s[i] >= '0' && s[i] <= '9';
        target = target * 10 + (uint64_t)(s[i] - '0');
    }
    size_t text_end = close ? (size_t)(close - s) : b;
    if (!ok || target > UINT32_MAX || text_end == sep + 1 || !plain_run(s, sep + 1, text_end, false)) {
        return writer_fail(w, why, "malformed link at byte %zu", a);
    }
    if (!w->doc_any_link || target > w->doc_max_link) {
        w->doc_max_link = (uint32_t)target;
    }
    w->doc_any_link = true;
    *end = text_end + 1;
    return add_span(w, a, *end) || writer_fail(w, why, "out of memory");
}

/* check_line: one line s[a, b) of document text (pack-v1.md "Document text"). */
static bool check_line(arcpack_writer *w, const char *s, size_t a, size_t b, const char **why) {
    if (a < b && s[a] == ARCPACK_MARK_HEADING) {
        if (b - a < 3 || s[a + 1] < '1' || s[a + 1] > '6' || !plain_run(s, a + 2, b, false)) {
            return writer_fail(w, why, "malformed heading at byte %zu", a);
        }
        return add_span(w, a, a + 2) || writer_fail(w, why, "out of memory");
    }
    if (a < b && s[a] == ARCPACK_MARK_IMAGE) {
        size_t sep = find_sep(s, a + 1, b);
        if (sep == b || sep == a + 1 || !plain_run(s, a + 1, sep, false) || !plain_run(s, sep + 1, b, false)) {
            return writer_fail(w, why, "malformed image line at byte %zu", a);
        }
        return add_span(w, a, sep + 1) || writer_fail(w, why, "out of memory");
    }
    for (size_t i = a; i < b; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ARCPACK_MARK_LINK) {
            size_t end;
            if (!check_link(w, s, i, b, &end, why)) {
                return false;
            }
            i = end - 1;
        } else if (is_control(c) && c != '\t') {
            return writer_fail(w, why, "control character 0x%02x at byte %zu", c, i);
        }
    }
    return true;
}

/* check_text: UTF-8 and markup of the whole text; fills w->spans in order. */
static bool check_text(arcpack_writer *w, const char *s, size_t len, const char **why) {
    w->n_spans = 0;
    w->doc_any_link = false;
    if (!arcjson_utf8_valid(s, len)) {
        return writer_fail(w, why, "text is not valid UTF-8");
    }
    for (size_t a = 0; a < len;) {
        const char *nl = memchr(s + a, '\n', len - a);
        size_t b = nl ? (size_t)(nl - s) : len;
        if (!check_line(w, s, a, b, why)) {
            return false;
        }
        a = b + 1;
    }
    return true;
}

/* cut_ok: a chunk may start or end at byte p: a character boundary outside every markup construct. */
static bool cut_ok(const arcpack_writer *w, const char *s, size_t len, size_t p) {
    if (p < len && ((unsigned char)s[p] & 0xC0) == 0x80) {
        return false;
    }
    /* The last span starting before p (spans are in order and don't overlap). */
    size_t lo = 0;
    size_t hi = w->n_spans;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (w->spans[mid].start < p) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo == 0 || p >= w->spans[lo - 1].end;
}

/* check_section: parts separated by ARCPACK_MARK_SEP, none empty, no other control characters. */
static bool check_section(const char *s, size_t len) {
    if (len == 0) {
        return true;
    }
    if (!arcjson_utf8_valid(s, len) || s[0] == ARCPACK_MARK_SEP || s[len - 1] == ARCPACK_MARK_SEP) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ARCPACK_MARK_SEP ? s[i + 1] == ARCPACK_MARK_SEP : is_control((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* check_doc: everything about one document before any byte of it is written. */
static bool check_doc(arcpack_writer *w, const char *title, size_t title_len, const char *text, size_t text_len,
                      const arcpack_chunk_spec *chunks, size_t n_chunks, const char **why) {
    if (w->n_docs == UINT32_MAX || n_chunks > UINT32_MAX - (uint64_t)w->n_chunks) {
        return writer_fail(w, why, "the pack is full (pack-v1 counts documents and chunks in 32 bits)");
    }
    if (text_len > UINT32_MAX || ZSTD_compressBound(text_len) > UINT32_MAX) {
        return writer_fail(w, why, "document text larger than pack-v1 allows");
    }
    if (title_len == 0 || !arcjson_utf8_valid(title, title_len) || !plain_run(title, 0, title_len, false)) {
        return writer_fail(w, why, "title empty, not UTF-8, or holding control characters");
    }
    uint64_t strings = title_len;
    if (!check_text(w, text, text_len, why)) {
        return false;
    }
    for (size_t i = 0; i < n_chunks; i++) {
        const arcpack_chunk_spec *c = &chunks[i];
        uint64_t end = (uint64_t)c->offset + c->length;
        if (c->length == 0 || end > text_len) {
            return writer_fail(w, why, "chunk %zu: range outside the text", i);
        }
        if (!cut_ok(w, text, text_len, c->offset) || !cut_ok(w, text, text_len, (size_t)end)) {
            return writer_fail(w, why, "chunk %zu: cut inside a character or markup", i);
        }
        if (!check_section(c->section, c->section ? c->section_length : 0)) {
            return writer_fail(w, why, "chunk %zu: malformed section path", i);
        }
        strings += c->section ? c->section_length : 0;
    }
    if (strings > UINT32_MAX - w->strings_len) {
        return writer_fail(w, why, "content/strings would pass 4 GiB");
    }
    return true;
}

/* ---- Writing ------------------------------------------------------------------------------------ */

/* put: append to file f; an error fails the writer. */
static bool put(arcpack_writer *w, int f, const void *data, size_t n, const char **why) {
    if (n != 0 && fwrite(data, 1, n, w->f[f]) != n) {
        w->failed = true;
        return writer_fail(w, why, "%s: write failed", arcpack_files[f].path);
    }
    return true;
}

/* put_string: append to content/strings; *offset is where it starts. */
static bool put_string(arcpack_writer *w, const char *s, size_t len, uint32_t *offset, const char **why) {
    *offset = len ? (uint32_t)w->strings_len : 0;
    w->strings_len += len;
    return put(w, ARCPACK_FILE_STRINGS, s, len, why);
}

/* put_section: the offset of section path s, written unless it equals the previous chunk's. */
static bool put_section(arcpack_writer *w, const char *s, size_t len, uint32_t *offset, const char **why) {
    if (len == 0) {
        *offset = 0;
        return true;
    }
    if (w->have_last_section && len == w->last_section_len && memcmp(s, w->last_section, len) == 0) {
        *offset = w->last_section_off;
        return true;
    }
    if (len > w->last_section_cap) {
        char *b = realloc(w->last_section, len);
        if (b == NULL) {
            w->failed = true;
            return writer_fail(w, why, "out of memory");
        }
        w->last_section = b;
        w->last_section_cap = len;
    }
    memcpy(w->last_section, s, len);
    w->last_section_len = len;
    w->have_last_section = put_string(w, s, len, &w->last_section_off, why);
    *offset = w->last_section_off;
    return w->have_last_section;
}

/* put_frame: compress text as one frame onto content/docs.zst; *size is the frame's size. */
static bool put_frame(arcpack_writer *w, const char *text, size_t len, uint32_t *size, const char **why) {
    uint64_t total = 0;
    ZSTD_inBuffer in = {text, len, 0};
    size_t left = ZSTD_CCtx_reset(w->cctx, ZSTD_reset_session_only);
    if (!ZSTD_isError(left)) {
        left = ZSTD_CCtx_setPledgedSrcSize(w->cctx, len);
    }
    while (!ZSTD_isError(left)) {
        ZSTD_outBuffer out = {w->zbuf, w->zbuf_cap, 0};
        left = ZSTD_compressStream2(w->cctx, &out, &in, ZSTD_e_end);
        if (!ZSTD_isError(left) && !put(w, ARCPACK_FILE_ZST, w->zbuf, out.pos, why)) {
            return false;
        }
        total += out.pos;
        if (left == 0) {
            break;
        }
    }
    if (ZSTD_isError(left)) {
        w->failed = true;
        return writer_fail(w, why, "zstd: %s", ZSTD_getErrorName(left));
    }
    *size = (uint32_t)total; /* <= ZSTD_compressBound(len) <= UINT32_MAX, checked first */
    return true;
}

bool arcpack_writer_add_doc(arcpack_writer *w, const char *title, size_t title_len, const char *text, size_t text_len,
                            const arcpack_chunk_spec *chunks, size_t n_chunks, const char **why) {
    if (w->failed) {
        *why = w->err;
        return false;
    }
    if (!check_doc(w, title, title_len, text, text_len, chunks, n_chunks, why)) {
        return false;
    }
    arcpack_doc_record d = {
        .frame_offset = ARCPACK_HEADER_SIZE + w->zst_pos,
        .text_size = (uint32_t)text_len,
        .title_length = (uint32_t)title_len,
        .first_chunk = w->n_chunks,
        .chunk_count = (uint32_t)n_chunks,
    };
    if (!put_string(w, title, title_len, &d.title_offset, why) || !put_frame(w, text, text_len, &d.frame_size, why)) {
        return false;
    }
    w->zst_pos += d.frame_size;
    for (size_t i = 0; i < n_chunks; i++) {
        const arcpack_chunk_spec *c = &chunks[i];
        arcpack_chunk_record r = {
            .document = w->n_docs,
            .offset = c->offset,
            .length = c->length,
            .section_length = c->section ? (uint32_t)c->section_length : 0,
            .tags = c->tags,
        };
        if (!put_section(w, c->section, r.section_length, &r.section_offset, why) ||
            !put(w, ARCPACK_FILE_CHUNKS, &r, sizeof(r), why)) {
            return false;
        }
    }
    if (!put(w, ARCPACK_FILE_DOCS, &d, sizeof(d), why)) {
        return false;
    }
    w->n_docs++;
    w->n_chunks += (uint32_t)n_chunks;
    if (w->doc_any_link && (!w->any_link || w->doc_max_link > w->max_link)) {
        w->max_link = w->doc_max_link;
        w->any_link = true;
    }
    return true;
}

uint32_t arcpack_writer_doc_count(const arcpack_writer *w) {
    return w->n_docs;
}

uint32_t arcpack_writer_chunk_count(const arcpack_writer *w) {
    return w->n_chunks;
}

/* ---- Finishing ---------------------------------------------------------------------------------- */

/* seal: write file f's header, flush, fsync and close it. */
static bool seal(arcpack_writer *w, int f, uint64_t count, const char **why) {
    arcpack_file_header h = {
        .schema_version = ARCPACK_SCHEMA_VERSION, .record_size = arcpack_files[f].record_size, .count = count};
    memcpy(h.magic, arcpack_files[f].magic, sizeof(h.magic));
    memcpy(h.pack_name, w->name, sizeof(h.pack_name));
    memcpy(h.pack_version, w->version, sizeof(h.pack_version));
    FILE *fp = w->f[f];
    bool ok = fseek(fp, 0, SEEK_SET) == 0 && fwrite(&h, 1, sizeof(h), fp) == sizeof(h) && fflush(fp) == 0 &&
              fsync(fileno(fp)) == 0;
    ok = (fclose(fp) == 0) && ok;
    w->f[f] = NULL;
    return ok || writer_fail(w, why, "%s: %s", arcpack_files[f].path, strerror(errno));
}

bool arcpack_writer_finish(arcpack_writer *w, const char **why) {
    static _Thread_local char finish_why[256];
    const uint64_t count[ARCPACK_NFILES] = {
        [ARCPACK_FILE_DOCS] = w->n_docs,
        [ARCPACK_FILE_STRINGS] = w->strings_len,
        [ARCPACK_FILE_ZST] = w->n_docs,
        [ARCPACK_FILE_CHUNKS] = w->n_chunks,
    };
    bool ok = !w->failed;
    if (ok && w->any_link && w->max_link >= w->n_docs) {
        ok = writer_fail(w, why, "a link names document %u, but the pack has %u", w->max_link, w->n_docs);
    }
    for (int f = 0; ok && f < ARCPACK_NFILES; f++) {
        ok = seal(w, f, count[f], why);
    }
    char from[PATH_MAX];
    char to[PATH_MAX];
    for (int f = 0; ok && f < ARCPACK_NFILES; f++) {
        ok = path_of(w, arcpack_files[f].path, ".part", from, sizeof(from)) &&
             path_of(w, arcpack_files[f].path, "", to, sizeof(to)) && rename(from, to) == 0;
        if (!ok) {
            writer_fail(w, why, "%s: rename: %s", arcpack_files[f].path, strerror(errno));
        }
    }
    if (ok && (!path_of(w, "content", "", from, sizeof(from)) || !arcutil_sync_dir(from) ||
               !path_of(w, "chunks", "", to, sizeof(to)) || !arcutil_sync_dir(to))) {
        ok = writer_fail(w, why, "fsync of the pack directories failed");
    }
    if (!ok) {
        snprintf(finish_why, sizeof(finish_why), "%s", w->err);
        *why = finish_why;
        close_files(w, true);
    } else {
        close_files(w, false);
    }
    writer_free(w);
    return ok;
}

void arcpack_writer_abort(arcpack_writer *w) {
    if (w == NULL) {
        return;
    }
    close_files(w, true);
    writer_free(w);
}
