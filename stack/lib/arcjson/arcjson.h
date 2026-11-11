/*
 * arcjson — the user-space stack's JSON parser and writer (M2-04, lane-l.md).
 *
 * Parser: RFC 8259, strict. UTF-8 is validated, escapes are decoded (surrogate pairs
 * combined, lone surrogates rejected), nesting is limited to ARCJSON_MAX_DEPTH, the input
 * size to a limit the caller gives, and nothing but whitespace may follow the value.
 * Every value, string and node lives in one arena, so a whole document is freed with
 * arcjson_arena_free(). Objects keep members in input order; arcjson_get() scans them
 * (objects here are small: requests and manifests), and a duplicate key finds the first.
 *
 * Writer: builds a document into one growing buffer; commas and nesting are handled by
 * the writer. Strings are escaped and invalid UTF-8 is replaced with U+FFFD, so the output
 * is always valid JSON even for raw model output.
 *
 * Manifests: reading a small JSON file whole, and the schemas' `id` and version rules, shared by
 * every program that reads manifests (arc-llm, arcpack, arcpkg).
 *
 * Cost: parse is O(input) time and about 2x input plus 40 bytes per value of arena memory.
 */
#ifndef ARCHOS_STACK_ARCJSON_H
#define ARCHOS_STACK_ARCJSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARCJSON_MAX_DEPTH 64

typedef enum {
    ARCJSON_NULL,
    ARCJSON_FALSE,
    ARCJSON_TRUE,
    ARCJSON_NUMBER,
    ARCJSON_STRING,
    ARCJSON_ARRAY,
    ARCJSON_OBJECT,
} arcjson_type;

typedef struct arcjson_value arcjson_value;

/* One parsed value. Arrays and objects hold a list of children linked through `next`. */
struct arcjson_value {
    arcjson_type type;
    const char *key; /* member name (NUL-terminated) when inside an object, else NULL */
    size_t key_len;
    arcjson_value *next; /* next element or member of the parent */
    union {
        double number;
        struct {
            const char *ptr; /* decoded UTF-8, NUL-terminated (may also contain NUL) */
            size_t len;
        } string;
        struct {
            arcjson_value *first;
            size_t count;
        } children;
    } u;
};

struct arcjson_chunk;

/* Bump allocator for one parsed document. Zero-initialise or call arcjson_arena_init. */
typedef struct {
    struct arcjson_chunk *head;
    size_t total; /* bytes allocated from the system, for budgets */
} arcjson_arena;

/* Where and why parsing stopped. */
typedef struct {
    size_t offset; /* byte offset into the input */
    const char *message;
} arcjson_error;

/* arcjson_arena_init: start an empty arena. */
void arcjson_arena_init(arcjson_arena *a);

/* arcjson_arena_free: free everything parsed into the arena; it can be reused afterwards. */
void arcjson_arena_free(arcjson_arena *a);

/*
 * arcjson_parse: parse `len` bytes of `text` (need not be NUL-terminated) into arena `a`.
 * Fails if len > max_len. Returns the root value, or NULL with *err filled in (err may be NULL).
 */
const arcjson_value *arcjson_parse(arcjson_arena *a, const char *text, size_t len, size_t max_len, arcjson_error *err);

/* arcjson_get: the first member of object `obj` named `key`; NULL if obj is not an object or has none. */
const arcjson_value *arcjson_get(const arcjson_value *obj, const char *key);

/* arcjson_string: the string's text if v is a string, else NULL. */
const char *arcjson_string(const arcjson_value *v);

/* arcjson_int64: true and *out set if v is a number with an integral value in int64 range (|v| <= 2^53). */
bool arcjson_int64(const arcjson_value *v, int64_t *out);

/* Writer. Zero-initialise or call arcjson_writer_init; free with arcjson_writer_free. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int depth;
    bool after_key;
    bool failed; /* out of memory, or nesting deeper than ARCJSON_MAX_DEPTH */
    unsigned char first[ARCJSON_MAX_DEPTH + 1];
} arcjson_writer;

/* arcjson_writer_init: start an empty document. */
void arcjson_writer_init(arcjson_writer *w);

/* arcjson_writer_free: free the buffer. */
void arcjson_writer_free(arcjson_writer *w);

/* arcjson_writer_reset: empty the document, keeping the buffer for reuse. */
void arcjson_writer_reset(arcjson_writer *w);

/* arcjson_writer_result: the NUL-terminated document and its length; NULL if any write failed. */
const char *arcjson_writer_result(const arcjson_writer *w, size_t *len);

/* Containers. Inside an object, call arcjson_write_key before each member's value. */
void arcjson_write_object_begin(arcjson_writer *w);
void arcjson_write_object_end(arcjson_writer *w);
void arcjson_write_array_begin(arcjson_writer *w);
void arcjson_write_array_end(arcjson_writer *w);

/* arcjson_write_key: the name of the next object member (NUL-terminated, escaped like a string). */
void arcjson_write_key(arcjson_writer *w, const char *key);

/* Scalars. string_n writes `len` bytes (invalid UTF-8 becomes U+FFFD). Non-finite numbers become null. */
void arcjson_write_string(arcjson_writer *w, const char *s);
void arcjson_write_string_n(arcjson_writer *w, const char *s, size_t len);
void arcjson_write_int(arcjson_writer *w, int64_t v);
void arcjson_write_number(arcjson_writer *w, double v); /* shortest round-trip form, up to 17 digits */
void arcjson_write_float(arcjson_writer *w, float v);   /* 9 significant digits: round-trips a float */
void arcjson_write_bool(arcjson_writer *w, bool v);
void arcjson_write_null(arcjson_writer *w);

/*
 * arcjson_utf8_complete_len: len minus a trailing character that is valid so far but cut
 * short (at most 3 bytes). A streamer writes that many bytes now and keeps the rest until
 * the next piece completes the character. Invalid bytes are not held back.
 */
size_t arcjson_utf8_complete_len(const char *s, size_t len);

/*
 * arcjson_utf8_valid: true if s[0..len) is valid UTF-8 (shortest forms, no surrogates, at most
 * U+10FFFF), the same rule the parser applies. The stack's one UTF-8 check (ADR-0013).
 */
bool arcjson_utf8_valid(const char *s, size_t len);

/* ---- Manifests (schemas/) ------------------------------------------------------------------ */

/*
 * arcjson_read_file: the whole file at `path`, at most max_len bytes, in a malloc'd buffer with a
 * NUL after it, ready for arcjson_parse (the stack reads only small JSON files whole: manifests,
 * roles.json). NULL with *why set to static text (strerror, "larger than the size limit",
 * "out of memory") on failure.
 */
char *arcjson_read_file(const char *path, size_t max_len, size_t *len, const char **why);

/* arcjson_is_schema_id: s[0..len) is a common.schema.json `id`: a-z, 0-9, '-', not starting with '-', 1..64 bytes. */
bool arcjson_is_schema_id(const char *s, size_t len);

/*
 * arcjson_is_schema_version: s[0..len) starts with a digit and holds only semver's characters
 * (0-9, A-Z, a-z, '.', '-', '+'), 1..64 bytes: safe as a directory name and in a pack header.
 * (The full semver grammar is checked by the schema at build time.)
 */
bool arcjson_is_schema_version(const char *s, size_t len);

#endif /* ARCHOS_STACK_ARCJSON_H */
