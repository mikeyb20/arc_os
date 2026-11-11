/*
 * arcpack.c — the pack reader (see arcpack.h, format in docs/formats/pack-v1.md), plain text,
 * and the file table the writer shares (arcpack_internal.h).
 *
 * The four binary files are mapped whole and read in place. arcpack_open() checks the headers,
 * the file sizes and the manifest; each accessor checks the one record it reads against the
 * mapped sizes, so the cost of opening does not grow with the pack.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include "arcjson.h"
#include "arcpack_internal.h"

#define MANIFEST_MAX (4u << 20) /* bytes; manifests are a few KiB */

const arcpack_file_kind arcpack_files[ARCPACK_NFILES] = {
    [ARCPACK_FILE_DOCS] = {"content/docs.idx", "ARCPDOCS", sizeof(arcpack_doc_record)},
    [ARCPACK_FILE_STRINGS] = {"content/strings", "ARCPSTRS", 1},
    /* A zstd skippable frame header (magic 0x184D2A5A, 4088 bytes follow): docs.zst stays a zstd stream. */
    [ARCPACK_FILE_ZST] = {"content/docs.zst", {0x5A, 0x2A, 0x4D, 0x18, (char)0xF8, 0x0F, 0x00, 0x00}, 0},
    [ARCPACK_FILE_CHUNKS] = {"chunks/chunks.idx", "ARCPCHNK", sizeof(arcpack_chunk_record)},
};

struct arcpack {
    struct {
        const unsigned char *base;
        size_t size;
    } map[ARCPACK_NFILES];
    uint32_t n_docs;
    uint32_t n_chunks;
    uint64_t strings_len;
    ZSTD_DCtx *dctx;
    arcjson_arena arena; /* the parsed manifest: tag names point into it */
    char name[ARCPACK_NAME_MAX + 1];
    char version[ARCPACK_NAME_MAX + 1];
    const char *tags[ARCPACK_MAX_TAGS];
    size_t n_tags;
    char err[256];
};

/* ---- Errors ------------------------------------------------------------------------------------- */

static _Thread_local char open_why[512];

/* pack_fail: format the error of `p` and return false. */
__attribute__((format(printf, 2, 3))) static bool pack_fail(arcpack *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    return false;
}

const char *arcpack_error(const arcpack *p) {
    return p->err;
}

/* ---- Opening ------------------------------------------------------------------------------------ */

/* copy_id: a manifest string checked by `valid`, into dst (NUL-terminated). */
static bool copy_id(arcpack *p, const arcjson_value *v, const char *field, bool (*valid)(const char *, size_t),
                    char *dst) {
    if (v == NULL || v->type != ARCJSON_STRING || !valid(v->u.string.ptr, v->u.string.len)) {
        return pack_fail(p, "manifest.json: `%s` missing or not valid", field);
    }
    memcpy(dst, v->u.string.ptr, v->u.string.len);
    dst[v->u.string.len] = '\0';
    return true;
}

/* load_pack_manifest: name, version and domain tags from manifest.json. Counts are checked by check_counts. */
static bool load_pack_manifest(arcpack *p, const char *dir, const arcjson_value **root) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/manifest.json", dir) >= (int)sizeof(path)) {
        return pack_fail(p, "manifest.json: path too long");
    }
    size_t len = 0;
    const char *err;
    char *text = arcjson_read_file(path, MANIFEST_MAX, &len, &err);
    if (text == NULL) {
        return pack_fail(p, "manifest.json: %s", err);
    }
    arcjson_error jerr;
    *root = arcjson_parse(&p->arena, text, len, MANIFEST_MAX, &jerr);
    free(text);
    if (*root == NULL) {
        return pack_fail(p, "manifest.json: %s at byte %zu", jerr.message, jerr.offset);
    }
    int64_t schema;
    if ((*root)->type != ARCJSON_OBJECT || !arcjson_int64(arcjson_get(*root, "schema_version"), &schema) ||
        schema != ARCPACK_SCHEMA_VERSION) {
        return pack_fail(p, "manifest.json: not a schema_version %d pack manifest", ARCPACK_SCHEMA_VERSION);
    }
    if (!copy_id(p, arcjson_get(*root, "name"), "name", arcjson_is_schema_id, p->name) ||
        !copy_id(p, arcjson_get(*root, "version"), "version", arcjson_is_schema_version, p->version)) {
        return false;
    }
    const arcjson_value *tags = arcjson_get(*root, "domain_tags");
    if (tags == NULL || tags->type != ARCJSON_ARRAY || tags->u.children.count > ARCPACK_MAX_TAGS) {
        return pack_fail(p, "manifest.json: `domain_tags` missing or longer than %u", ARCPACK_MAX_TAGS);
    }
    for (const arcjson_value *t = tags->u.children.first; t != NULL; t = t->next) {
        if (t->type != ARCJSON_STRING) {
            return pack_fail(p, "manifest.json: `domain_tags` holds a non-string");
        }
        p->tags[p->n_tags++] = t->u.string.ptr;
    }
    return true;
}

/* map_file: map one binary file and check its header against the manifest. */
static bool map_file(arcpack *p, const char *dir, int f, uint64_t *count) {
    const arcpack_file_kind *k = &arcpack_files[f];
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, k->path) >= (int)sizeof(path)) {
        return pack_fail(p, "%s: path too long", k->path);
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        pack_fail(p, "%s: %s", k->path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < (off_t)ARCPACK_HEADER_SIZE) {
        close(fd);
        return pack_fail(p, "%s: shorter than its %u-byte header", k->path, ARCPACK_HEADER_SIZE);
    }
    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        return pack_fail(p, "%s: mmap: %s", k->path, strerror(errno));
    }
    p->map[f].base = base;
    p->map[f].size = (size_t)st.st_size;

    arcpack_file_header h;
    memcpy(&h, base, sizeof(h));
    char name[ARCPACK_NAME_MAX] = {0};
    char version[ARCPACK_NAME_MAX] = {0};
    memcpy(name, p->name, strlen(p->name));
    memcpy(version, p->version, strlen(p->version));
    if (memcmp(h.magic, k->magic, sizeof(h.magic)) != 0) {
        return pack_fail(p, "%s: not a pack file (bad magic)", k->path);
    }
    if (h.schema_version != ARCPACK_SCHEMA_VERSION) {
        return pack_fail(p, "%s: schema_version %u, this reader knows %d", k->path, h.schema_version,
                         ARCPACK_SCHEMA_VERSION);
    }
    if (h.record_size != k->record_size) {
        return pack_fail(p, "%s: record_size %u, expected %u", k->path, h.record_size, k->record_size);
    }
    if (memcmp(h.pack_name, name, sizeof(name)) != 0 || memcmp(h.pack_version, version, sizeof(version)) != 0) {
        return pack_fail(p, "%s: belongs to another pack or version than manifest.json (%s %s)", k->path, p->name,
                         p->version);
    }
    uint64_t data = (uint64_t)st.st_size - ARCPACK_HEADER_SIZE;
    if (k->record_size != 0 && (data % k->record_size != 0 || data / k->record_size != h.count)) {
        return pack_fail(p, "%s: %llu data bytes, but the header counts %llu records of %u", k->path,
                         (unsigned long long)data, (unsigned long long)h.count, k->record_size);
    }
    *count = h.count;
    return true;
}

/* check_counts: the files and the manifest agree on how many documents and chunks there are. */
static bool check_counts(arcpack *p, const arcjson_value *manifest, const uint64_t count[ARCPACK_NFILES]) {
    if (count[ARCPACK_FILE_DOCS] > UINT32_MAX || count[ARCPACK_FILE_CHUNKS] > UINT32_MAX ||
        count[ARCPACK_FILE_STRINGS] > UINT32_MAX) {
        return pack_fail(p, "more documents, chunks or string bytes than pack-v1 allows");
    }
    if (count[ARCPACK_FILE_ZST] != count[ARCPACK_FILE_DOCS]) {
        return pack_fail(p, "content/docs.zst counts %llu frames, content/docs.idx %llu documents",
                         (unsigned long long)count[ARCPACK_FILE_ZST], (unsigned long long)count[ARCPACK_FILE_DOCS]);
    }
    p->n_docs = (uint32_t)count[ARCPACK_FILE_DOCS];
    p->n_chunks = (uint32_t)count[ARCPACK_FILE_CHUNKS];
    p->strings_len = count[ARCPACK_FILE_STRINGS];
    int64_t n;
    if (!arcjson_int64(arcjson_get(manifest, "chunk_count"), &n) || n != (int64_t)p->n_chunks) {
        return pack_fail(p, "manifest.json: `chunk_count` is not the %u chunks of chunks/chunks.idx", p->n_chunks);
    }
    const arcjson_value *docs = arcjson_get(manifest, "document_count");
    if (docs != NULL && (!arcjson_int64(docs, &n) || n != (int64_t)p->n_docs)) {
        return pack_fail(p, "manifest.json: `document_count` is not the %u documents of content/docs.idx", p->n_docs);
    }
    return true;
}

arcpack *arcpack_open(const char *dir, const char **why) {
    arcpack *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        *why = "out of memory";
        return NULL;
    }
    arcjson_arena_init(&p->arena);
    const arcjson_value *manifest = NULL;
    uint64_t count[ARCPACK_NFILES];
    bool ok = load_pack_manifest(p, dir, &manifest);
    for (int f = 0; ok && f < ARCPACK_NFILES; f++) {
        ok = map_file(p, dir, f, &count[f]);
    }
    ok = ok && check_counts(p, manifest, count);
    if (ok) {
        p->dctx = ZSTD_createDCtx();
        ok = p->dctx != NULL || pack_fail(p, "out of memory");
    }
    if (!ok) {
        int n = snprintf(open_why, sizeof(open_why), "%s: %s", dir, p->err);
        (void)n; /* a long path may cut the message short; it stays NUL-terminated */
        *why = open_why;
        arcpack_close(p);
        return NULL;
    }
    return p;
}

void arcpack_close(arcpack *p) {
    if (p == NULL) {
        return;
    }
    for (int f = 0; f < ARCPACK_NFILES; f++) {
        if (p->map[f].base != NULL) {
            munmap((void *)p->map[f].base, p->map[f].size);
        }
    }
    ZSTD_freeDCtx(p->dctx);
    arcjson_arena_free(&p->arena);
    free(p);
}

/* ---- Manifest values ---------------------------------------------------------------------------- */

const char *arcpack_name(const arcpack *p) {
    return p->name;
}

const char *arcpack_version(const arcpack *p) {
    return p->version;
}

size_t arcpack_tag_count(const arcpack *p) {
    return p->n_tags;
}

const char *arcpack_tag_name(const arcpack *p, size_t bit) {
    return bit < p->n_tags ? p->tags[bit] : NULL;
}

uint32_t arcpack_doc_count(const arcpack *p) {
    return p->n_docs;
}

uint32_t arcpack_chunk_count(const arcpack *p) {
    return p->n_chunks;
}

/* ---- Documents and chunks ----------------------------------------------------------------------- */

/* string_at: strings[offset, offset + length), if inside the strings file. */
static const char *string_at(const arcpack *p, uint32_t offset, uint32_t length) {
    if ((uint64_t)offset + length > p->strings_len) {
        return NULL;
    }
    return (const char *)p->map[ARCPACK_FILE_STRINGS].base + ARCPACK_HEADER_SIZE + offset;
}

/* doc_record: document `doc`'s record, every field checked against the files. */
static bool doc_record(arcpack *p, uint32_t doc, arcpack_doc_record *r) {
    if (doc >= p->n_docs) {
        return pack_fail(p, "document %u: out of range (the pack has %u)", doc, p->n_docs);
    }
    memcpy(r, p->map[ARCPACK_FILE_DOCS].base + ARCPACK_HEADER_SIZE + (size_t)doc * sizeof(*r), sizeof(*r));
    size_t zst_size = p->map[ARCPACK_FILE_ZST].size;
    if (r->frame_offset < ARCPACK_HEADER_SIZE || r->frame_offset > zst_size || r->frame_size == 0 ||
        r->frame_size > zst_size - r->frame_offset) {
        return pack_fail(p, "document %u: frame outside content/docs.zst", doc);
    }
    if (string_at(p, r->title_offset, r->title_length) == NULL) {
        return pack_fail(p, "document %u: title outside content/strings", doc);
    }
    if ((uint64_t)r->first_chunk + r->chunk_count > p->n_chunks) {
        return pack_fail(p, "document %u: chunks outside chunks/chunks.idx", doc);
    }
    return true;
}

bool arcpack_doc_get(arcpack *p, uint32_t doc, arcpack_doc *out) {
    arcpack_doc_record r;
    if (!doc_record(p, doc, &r)) {
        return false;
    }
    out->title = string_at(p, r.title_offset, r.title_length);
    out->title_length = r.title_length;
    out->text_size = r.text_size;
    out->first_chunk = r.first_chunk;
    out->chunk_count = r.chunk_count;
    return true;
}

/*
 * plain_frame: the frame header follows pack-v1.md: content checksum on, no dictionary, window at
 * most 8 MiB. Frame header descriptor (RFC 8878 3.1.1.1.1): bit 5 single segment (the window is
 * then the content size; otherwise a window descriptor byte follows, 3.1.1.1.2), bit 2 checksum,
 * bits 0-1 dictionary ID size. Checked by hand: zstd's one-shot decoder ignores ZSTD_d_windowLogMax.
 */
static bool plain_frame(const unsigned char *frame, size_t size) {
    uint32_t magic;
    if (size < 6) {
        return false;
    }
    memcpy(&magic, frame, sizeof(magic));
    unsigned d = frame[4];
    if (magic != ZSTD_MAGICNUMBER || (d & 0x04) == 0 || (d & 0x03) != 0) {
        return false;
    }
    uint64_t window = ZSTD_getFrameContentSize(frame, size); /* error codes are huge: refused */
    if ((d & 0x20) == 0) {
        uint64_t base = 1ull << (10 + (frame[5] >> 3));
        window = base + (base >> 3) * (frame[5] & 7u);
    }
    return window <= (1ull << ARCPACK_ZSTD_WINDOW_LOG);
}

bool arcpack_doc_text(arcpack *p, uint32_t doc, char *buf, size_t cap, size_t *len) {
    arcpack_doc_record r;
    if (!doc_record(p, doc, &r)) {
        return false;
    }
    if (cap < r.text_size) {
        return pack_fail(p, "document %u: needs a %u-byte buffer", doc, r.text_size);
    }
    const unsigned char *frame = p->map[ARCPACK_FILE_ZST].base + r.frame_offset;
    if (!plain_frame(frame, r.frame_size)) {
        return pack_fail(
            p, "document %u: not a plain pack-v1 frame (checksum off, a dictionary or a window over 8 MiB)", doc);
    }
    /* Exactly one frame, whose header states the size the record gives. */
    if (ZSTD_findFrameCompressedSize(frame, r.frame_size) != r.frame_size ||
        ZSTD_getFrameContentSize(frame, r.frame_size) != r.text_size) {
        return pack_fail(p, "document %u: damaged frame header", doc);
    }
    size_t n = ZSTD_decompressDCtx(p->dctx, buf, r.text_size, frame, r.frame_size);
    if (ZSTD_isError(n) || n != r.text_size) {
        return pack_fail(p, "document %u: damaged frame (%s)", doc, ZSTD_isError(n) ? ZSTD_getErrorName(n) : "size");
    }
    *len = n;
    return true;
}

bool arcpack_chunk_get(arcpack *p, uint32_t index, arcpack_chunk *out) {
    if (index >= p->n_chunks) {
        return pack_fail(p, "chunk %u: out of range (the pack has %u)", index, p->n_chunks);
    }
    arcpack_chunk_record c;
    memcpy(&c, p->map[ARCPACK_FILE_CHUNKS].base + ARCPACK_HEADER_SIZE + (size_t)index * sizeof(c), sizeof(c));
    arcpack_doc_record d;
    if (!doc_record(p, c.document, &d)) {
        return false;
    }
    if (index < d.first_chunk || index - d.first_chunk >= d.chunk_count) {
        return pack_fail(p, "chunk %u: not among document %u's chunks", index, c.document);
    }
    if (c.length == 0 || (uint64_t)c.offset + c.length > d.text_size) {
        return pack_fail(p, "chunk %u: range outside document %u's text", index, c.document);
    }
    const char *section = string_at(p, c.section_offset, c.section_length);
    if (section == NULL) {
        return pack_fail(p, "chunk %u: section path outside content/strings", index);
    }
    out->document = c.document;
    out->number = index - d.first_chunk;
    out->offset = c.offset;
    out->length = c.length;
    out->doc_text_size = d.text_size;
    out->section = section;
    out->section_length = c.section_length;
    out->tags = c.tags;
    return true;
}

bool arcpack_chunk_text(arcpack *p, uint32_t index, char *buf, size_t cap, size_t *len) {
    arcpack_chunk c;
    size_t text_len;
    if (!arcpack_chunk_get(p, index, &c) || !arcpack_doc_text(p, c.document, buf, cap, &text_len)) {
        return false;
    }
    memmove(buf, buf + c.offset, c.length);
    *len = c.length;
    return true;
}

/* ---- Chunk IDs ---------------------------------------------------------------------------------- */

bool arcpack_chunk_id(arcpack *p, uint32_t index, char *buf, size_t cap) {
    arcpack_chunk c;
    if (!arcpack_chunk_get(p, index, &c)) {
        return false;
    }
    int n = snprintf(buf, cap, "%s:%u:%u", p->name, c.document, c.number);
    return (n >= 0 && (size_t)n < cap) || pack_fail(p, "chunk %u: ID buffer too small", index);
}

/* parse_u32: decimal digits s[*i..) up to `stop` or the end, no sign, no leading zeros; advances *i. */
static bool parse_u32(const char *s, size_t len, size_t *i, char stop, uint32_t *out) {
    size_t start = *i;
    uint64_t v = 0;
    while (*i < len && s[*i] != stop) {
        if (s[*i] < '0' || s[*i] > '9' || *i - start >= 10) {
            return false;
        }
        v = v * 10 + (uint64_t)(s[*i] - '0');
        (*i)++;
    }
    size_t digits = *i - start;
    if (digits == 0 || (digits > 1 && s[start] == '0') || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

bool arcpack_chunk_parse(arcpack *p, const char *id, size_t len, uint32_t *index) {
    size_t name_len = strlen(p->name);
    if (len <= name_len || memcmp(id, p->name, name_len) != 0 || id[name_len] != ':') {
        return pack_fail(p, "chunk ID '%.*s': not of pack %s", (int)(len < 100 ? len : 100), id, p->name);
    }
    size_t i = name_len + 1;
    uint32_t doc;
    uint32_t number;
    bool ok = parse_u32(id, len, &i, ':', &doc) && i < len;
    if (ok) {
        i++; /* the ':' */
        ok = parse_u32(id, len, &i, '\0', &number) && i == len;
    }
    if (!ok) {
        return pack_fail(p, "chunk ID '%.*s': not pack:document:chunk", (int)(len < 100 ? len : 100), id);
    }
    arcpack_doc_record d;
    if (!doc_record(p, doc, &d)) {
        return false;
    }
    if (number >= d.chunk_count) {
        return pack_fail(p, "chunk ID %s:%u:%u: document %u has %u chunks", p->name, doc, number, doc, d.chunk_count);
    }
    *index = d.first_chunk + number;
    return true;
}

/* ---- Plain text --------------------------------------------------------------------------------- */

size_t arcpack_plain(char *dst, const char *src, size_t len) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == ARCPACK_MARK_HEADING) {
            if (i + 1 < len && src[i + 1] >= '1' && src[i + 1] <= '6') {
                i++; /* the level */
            }
        } else if (c == ARCPACK_MARK_LINK || c == ARCPACK_MARK_IMAGE) {
            /* Drop the link target or image reference, up to and including the separator. */
            while (i + 1 < len && src[i + 1] != ARCPACK_MARK_SEP && src[i + 1] != '\n') {
                i++;
            }
            if (i + 1 < len && src[i + 1] == ARCPACK_MARK_SEP) {
                i++;
            }
        } else if (c != ARCPACK_MARK_LINK_END && c != ARCPACK_MARK_SEP) {
            dst[o++] = c;
        }
    }
    return o;
}
