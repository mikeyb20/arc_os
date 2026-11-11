/*
 * arcjson.c — JSON parser and writer for the user-space stack (M2-04). See arcjson.h.
 *
 * The parser is recursive descent; recursion depth is bounded by ARCJSON_MAX_DEPTH, so the
 * stack use is too. All output goes into the caller's arena.
 */
#include "arcjson.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- arena ------------------------------------------------------------------------------ */

struct arcjson_chunk {
    struct arcjson_chunk *next;
    size_t size;
    size_t used;
    max_align_t data[];
};

#define ARENA_MIN_CHUNK 4096u
#define ARENA_MAX_CHUNK (1u << 20)
#define ARENA_ALIGN     (sizeof(max_align_t))

void arcjson_arena_init(arcjson_arena *a) {
    a->head = NULL;
    a->total = 0;
}

void arcjson_arena_free(arcjson_arena *a) {
    struct arcjson_chunk *c = a->head;
    while (c != NULL) {
        struct arcjson_chunk *next = c->next;
        free(c);
        c = next;
    }
    arcjson_arena_init(a);
}

/* arena_alloc: n bytes, aligned for any type; NULL when out of memory. */
static void *arena_alloc(arcjson_arena *a, size_t n) {
    n = (n + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
    struct arcjson_chunk *c = a->head;
    if (c == NULL || c->size - c->used < n) {
        size_t size = c == NULL ? ARENA_MIN_CHUNK : c->size * 2;
        if (size > ARENA_MAX_CHUNK) {
            size = ARENA_MAX_CHUNK;
        }
        if (size < n) {
            size = n;
        }
        struct arcjson_chunk *fresh = malloc(sizeof(*fresh) + size);
        if (fresh == NULL) {
            return NULL;
        }
        fresh->next = c;
        fresh->size = size;
        fresh->used = 0;
        a->head = fresh;
        a->total += sizeof(*fresh) + size;
        c = fresh;
    }
    void *p = (char *)c->data + c->used;
    c->used += n;
    return p;
}

/* ---- UTF-8 ------------------------------------------------------------------------------ */

/*
 * utf8_decode: decode one character from s[0..n). Returns its length (1-4) and sets *cp, or
 * 0 if the bytes are not a valid, complete, shortest-form character outside the surrogates.
 * If `truncated` is not NULL it is set when the bytes are a valid start cut off by n.
 */
static size_t utf8_decode(const unsigned char *s, size_t n, uint32_t *cp, bool *truncated) {
    if (truncated != NULL) {
        *truncated = false;
    }
    if (n == 0) {
        return 0;
    }
    unsigned char c = s[0];
    size_t len;
    uint32_t min;
    uint32_t v;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        min = 0x80;
        v = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        min = 0x800;
        v = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        min = 0x10000;
        v = c & 0x07;
    } else {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if (i >= n) {
            if (truncated != NULL) {
                *truncated = true;
            }
            return 0;
        }
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        v = (v << 6) | (s[i] & 0x3F);
        /* Reject overlong, surrogate and out-of-range forms as early as the second byte. */
        if (i == 1) {
            uint32_t top = v << (6 * (len - 2));
            if (top < min || (top >= 0xD800 && top <= 0xDFFF) || top > 0x10FFFF) {
                return 0;
            }
        }
    }
    *cp = v;
    return len;
}

size_t arcjson_utf8_complete_len(const char *s, size_t len) {
    /* A cut character starts at most 3 bytes before the end. */
    size_t start = len > 3 ? len - 3 : 0;
    for (size_t i = start; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xC2 && c <= 0xF4) {
            uint32_t cp;
            bool truncated;
            if (utf8_decode((const unsigned char *)s + i, len - i, &cp, &truncated) == 0 && truncated) {
                return i;
            }
        }
    }
    return len;
}

bool arcjson_utf8_valid(const char *s, size_t len) {
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        if (u[i] < 0x80) {
            i++; /* ASCII fast path */
            continue;
        }
        size_t n = utf8_decode(u + i, len - i, &cp, NULL);
        if (n == 0) {
            return false;
        }
        i += n;
    }
    return true;
}

size_t arcjson_utf8_decode(const char *s, size_t len, uint32_t *cp) {
    return utf8_decode((const unsigned char *)s, len, cp, NULL);
}

/* utf8_encode: write code point cp (valid, not a surrogate) to out; returns the length. */
static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- parser ----------------------------------------------------------------------------- */

typedef struct {
    const unsigned char *s;
    size_t len;
    size_t pos;
    arcjson_arena *arena;
    arcjson_error *err;
} parser;

static bool parse_fail(parser *p, const char *message) {
    if (p->err != NULL && p->err->message == NULL) {
        p->err->offset = p->pos;
        p->err->message = message;
    }
    return false;
}

static void parse_ws(parser *p) {
    while (p->pos < p->len) {
        unsigned char c = p->s[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        p->pos++;
    }
}

static bool parse_literal(parser *p, const char *word) {
    size_t n = strlen(word);
    if (p->len - p->pos < n || memcmp(p->s + p->pos, word, n) != 0) {
        return parse_fail(p, "invalid literal");
    }
    p->pos += n;
    return true;
}

static int parse_hex4(const unsigned char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = s[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return -1;
        }
        v = v * 16 + d;
    }
    return v;
}

/* parse_escape: after a backslash at p->pos; appends the decoded character to out. */
static bool parse_escape(parser *p, char *out, size_t *o) {
    if (p->pos >= p->len) {
        return parse_fail(p, "unterminated string");
    }
    unsigned char c = p->s[p->pos++];
    static const char from[] = "\"\\/bfnrt";
    static const char to[] = "\"\\/\b\f\n\r\t";
    const char *hit = c != 0 ? strchr(from, c) : NULL;
    if (hit != NULL) {
        out[(*o)++] = to[hit - from];
        return true;
    }
    if (c != 'u') {
        return parse_fail(p, "invalid escape");
    }
    if (p->len - p->pos < 4 || parse_hex4(p->s + p->pos) < 0) {
        return parse_fail(p, "invalid \\u escape");
    }
    uint32_t cp = (uint32_t)parse_hex4(p->s + p->pos);
    p->pos += 4;
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        return parse_fail(p, "lone low surrogate");
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (p->len - p->pos < 6 || p->s[p->pos] != '\\' || p->s[p->pos + 1] != 'u') {
            return parse_fail(p, "lone high surrogate");
        }
        int low = parse_hex4(p->s + p->pos + 2);
        if (low < 0xDC00 || low > 0xDFFF) {
            return parse_fail(p, "high surrogate without low surrogate");
        }
        p->pos += 6;
        cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)low - 0xDC00);
    }
    *o += utf8_encode(cp, out + *o);
    return true;
}

/* parse_string: at the opening quote. The decoded text never exceeds the raw text. */
static bool parse_string(parser *p, const char **str, size_t *str_len) {
    p->pos++; /* opening quote */
    size_t start = p->pos;
    size_t end = start;
    while (end < p->len && p->s[end] != '"') {
        end += p->s[end] == '\\' ? 2 : 1;
    }
    char *out = arena_alloc(p->arena, (end > p->len ? p->len : end) - start + 1);
    if (out == NULL) {
        return parse_fail(p, "out of memory");
    }
    size_t o = 0;
    for (;;) {
        if (p->pos >= p->len) {
            return parse_fail(p, "unterminated string");
        }
        unsigned char c = p->s[p->pos];
        if (c == '"') {
            p->pos++;
            break;
        } else if (c == '\\') {
            p->pos++;
            if (!parse_escape(p, out, &o)) {
                return false;
            }
        } else if (c < 0x20) {
            return parse_fail(p, "control character in string");
        } else {
            uint32_t cp;
            size_t n = utf8_decode(p->s + p->pos, p->len - p->pos, &cp, NULL);
            if (n == 0) {
                return parse_fail(p, "invalid UTF-8");
            }
            memcpy(out + o, p->s + p->pos, n);
            o += n;
            p->pos += n;
        }
    }
    out[o] = '\0';
    *str = out;
    *str_len = o;
    return true;
}

static bool parse_digit_at(const parser *p, size_t i) {
    return i < p->len && p->s[i] >= '0' && p->s[i] <= '9';
}

/* parse_number: RFC 8259 grammar checked here, then converted with strtod. */
static bool parse_number(parser *p, double *out) {
    size_t i = p->pos;
    if (i < p->len && p->s[i] == '-') {
        i++;
    }
    if (!parse_digit_at(p, i)) {
        return parse_fail(p, "invalid number");
    }
    if (p->s[i] == '0') {
        i++;
        if (parse_digit_at(p, i)) {
            return parse_fail(p, "leading zero in number");
        }
    } else {
        while (parse_digit_at(p, i)) {
            i++;
        }
    }
    if (i < p->len && p->s[i] == '.') {
        i++;
        if (!parse_digit_at(p, i)) {
            return parse_fail(p, "invalid fraction");
        }
        while (parse_digit_at(p, i)) {
            i++;
        }
    }
    if (i < p->len && (p->s[i] == 'e' || p->s[i] == 'E')) {
        i++;
        if (i < p->len && (p->s[i] == '+' || p->s[i] == '-')) {
            i++;
        }
        if (!parse_digit_at(p, i)) {
            return parse_fail(p, "invalid exponent");
        }
        while (parse_digit_at(p, i)) {
            i++;
        }
    }
    size_t n = i - p->pos;
    char *copy = arena_alloc(p->arena, n + 1);
    if (copy == NULL) {
        return parse_fail(p, "out of memory");
    }
    memcpy(copy, p->s + p->pos, n);
    copy[n] = '\0';
    errno = 0;
    double v = strtod(copy, NULL);
    if (errno == ERANGE && isinf(v)) {
        return parse_fail(p, "number out of range");
    }
    *out = v;
    p->pos = i;
    return true;
}

static bool parse_value(parser *p, arcjson_value *v, int depth);

/* parse_container: array or object at its opening bracket; children are linked in order. */
static bool parse_container(parser *p, arcjson_value *v, int depth, bool object) {
    if (depth >= ARCJSON_MAX_DEPTH) {
        return parse_fail(p, "nesting too deep");
    }
    v->type = object ? ARCJSON_OBJECT : ARCJSON_ARRAY;
    v->u.children.first = NULL;
    v->u.children.count = 0;
    arcjson_value **tail = &v->u.children.first;
    char close = object ? '}' : ']';
    p->pos++;
    parse_ws(p);
    if (p->pos < p->len && p->s[p->pos] == close) {
        p->pos++;
        return true;
    }
    for (;;) {
        arcjson_value *child = arena_alloc(p->arena, sizeof(*child));
        if (child == NULL) {
            return parse_fail(p, "out of memory");
        }
        child->key = NULL;
        child->key_len = 0;
        child->next = NULL;
        parse_ws(p);
        if (object) {
            if (p->pos >= p->len || p->s[p->pos] != '"') {
                return parse_fail(p, "expected member name");
            }
            if (!parse_string(p, &child->key, &child->key_len)) {
                return false;
            }
            parse_ws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') {
                return parse_fail(p, "expected ':'");
            }
            p->pos++;
        }
        if (!parse_value(p, child, depth + 1)) {
            return false;
        }
        *tail = child;
        tail = &child->next;
        v->u.children.count++;
        parse_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == close) {
            p->pos++;
            return true;
        }
        return parse_fail(p, object ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

static bool parse_value(parser *p, arcjson_value *v, int depth) {
    parse_ws(p);
    if (p->pos >= p->len) {
        return parse_fail(p, "unexpected end of input");
    }
    switch (p->s[p->pos]) {
    case '{':
        return parse_container(p, v, depth, true);
    case '[':
        return parse_container(p, v, depth, false);
    case '"':
        v->type = ARCJSON_STRING;
        return parse_string(p, &v->u.string.ptr, &v->u.string.len);
    case 't':
        v->type = ARCJSON_TRUE;
        return parse_literal(p, "true");
    case 'f':
        v->type = ARCJSON_FALSE;
        return parse_literal(p, "false");
    case 'n':
        v->type = ARCJSON_NULL;
        return parse_literal(p, "null");
    default:
        v->type = ARCJSON_NUMBER;
        return parse_number(p, &v->u.number);
    }
}

const arcjson_value *arcjson_parse(arcjson_arena *a, const char *text, size_t len, size_t max_len, arcjson_error *err) {
    parser p = {(const unsigned char *)text, len, 0, a, err};
    if (err != NULL) {
        err->offset = 0;
        err->message = NULL;
    }
    if (len > max_len) {
        parse_fail(&p, "input too large");
        return NULL;
    }
    arcjson_value *root = arena_alloc(a, sizeof(*root));
    if (root == NULL) {
        parse_fail(&p, "out of memory");
        return NULL;
    }
    root->key = NULL;
    root->key_len = 0;
    root->next = NULL;
    if (!parse_value(&p, root, 0)) {
        return NULL;
    }
    parse_ws(&p);
    if (p.pos != p.len) {
        parse_fail(&p, "trailing characters after the value");
        return NULL;
    }
    return root;
}

const arcjson_value *arcjson_get(const arcjson_value *obj, const char *key) {
    if (obj == NULL || obj->type != ARCJSON_OBJECT) {
        return NULL;
    }
    size_t n = strlen(key);
    for (const arcjson_value *m = obj->u.children.first; m != NULL; m = m->next) {
        if (m->key_len == n && memcmp(m->key, key, n) == 0) {
            return m;
        }
    }
    return NULL;
}

const char *arcjson_string(const arcjson_value *v) {
    return v != NULL && v->type == ARCJSON_STRING ? v->u.string.ptr : NULL;
}

bool arcjson_int64(const arcjson_value *v, int64_t *out) {
    if (v == NULL || v->type != ARCJSON_NUMBER) {
        return false;
    }
    double d = v->u.number;
    if (!(d >= -9007199254740992.0 && d <= 9007199254740992.0) || d != floor(d)) {
        return false;
    }
    *out = (int64_t)d;
    return true;
}

/* ---- writer ----------------------------------------------------------------------------- */

void arcjson_writer_init(arcjson_writer *w) {
    memset(w, 0, sizeof(*w));
    w->first[0] = 1;
}

void arcjson_writer_free(arcjson_writer *w) {
    free(w->buf);
    arcjson_writer_init(w);
}

void arcjson_writer_reset(arcjson_writer *w) {
    char *buf = w->buf;
    size_t cap = w->cap;
    arcjson_writer_init(w);
    w->buf = buf;
    w->cap = cap;
    if (buf != NULL) {
        buf[0] = '\0';
    }
}

const char *arcjson_writer_result(const arcjson_writer *w, size_t *len) {
    if (w->failed || w->depth != 0 || w->buf == NULL) {
        return NULL;
    }
    if (len != NULL) {
        *len = w->len;
    }
    return w->buf;
}

/* w_put: append n bytes, keeping the buffer NUL-terminated. */
static void w_put(arcjson_writer *w, const char *s, size_t n) {
    if (w->failed) {
        return;
    }
    if (w->cap - w->len < n + 1) {
        size_t cap = w->cap ? w->cap : 256;
        while (cap - w->len < n + 1) {
            cap *= 2;
        }
        char *buf = realloc(w->buf, cap);
        if (buf == NULL) {
            w->failed = true;
            return;
        }
        w->buf = buf;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

/* w_before_value: the comma between siblings (none right after a key). */
static void w_before_value(arcjson_writer *w) {
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (!w->first[w->depth]) {
        w_put(w, ",", 1);
    }
    w->first[w->depth] = 0;
}

static void w_open(arcjson_writer *w, char c) {
    w_before_value(w);
    if (w->depth >= ARCJSON_MAX_DEPTH) {
        w->failed = true;
        return;
    }
    w_put(w, &c, 1);
    w->first[++w->depth] = 1;
}

static void w_close(arcjson_writer *w, char c) {
    if (w->depth == 0) {
        w->failed = true;
        return;
    }
    w->depth--;
    w_put(w, &c, 1);
}

void arcjson_write_object_begin(arcjson_writer *w) {
    w_open(w, '{');
}

void arcjson_write_object_end(arcjson_writer *w) {
    w_close(w, '}');
}

void arcjson_write_array_begin(arcjson_writer *w) {
    w_open(w, '[');
}

void arcjson_write_array_end(arcjson_writer *w) {
    w_close(w, ']');
}

/* w_put_string: quoted, escaped; invalid UTF-8 bytes become U+FFFD. */
static void w_put_string(arcjson_writer *w, const char *s, size_t len) {
    static const char replacement[] = "\xEF\xBF\xBD";
    w_put(w, "\"", 1);
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', (char)c};
            w_put(w, esc, 2);
            i++;
        } else if (c < 0x20) {
            static const char named[] = "\b\f\n\r\t", letters[] = "bfnrt"; /* the short escapes */
            const char *k = c != 0 ? strchr(named, c) : NULL;
            char esc[8];
            int n =
                k ? snprintf(esc, sizeof(esc), "\\%c", letters[k - named]) : snprintf(esc, sizeof(esc), "\\u%04x", c);
            w_put(w, esc, (size_t)n);
            i++;
        } else if (c < 0x80) {
            size_t run = i;
            while (run < len && (unsigned char)s[run] >= 0x20 && (unsigned char)s[run] < 0x80 && s[run] != '"' &&
                   s[run] != '\\') {
                run++;
            }
            w_put(w, s + i, run - i);
            i = run;
        } else {
            uint32_t cp;
            size_t n = utf8_decode((const unsigned char *)s + i, len - i, &cp, NULL);
            if (n == 0) {
                w_put(w, replacement, 3);
                i++;
            } else {
                w_put(w, s + i, n);
                i += n;
            }
        }
    }
    w_put(w, "\"", 1);
}

void arcjson_write_key(arcjson_writer *w, const char *key) {
    if (w->depth == 0) {
        w->failed = true;
        return;
    }
    w_before_value(w);
    w_put_string(w, key, strlen(key));
    w_put(w, ":", 1);
    w->after_key = true;
}

void arcjson_write_string(arcjson_writer *w, const char *s) {
    arcjson_write_string_n(w, s, strlen(s));
}

void arcjson_write_string_n(arcjson_writer *w, const char *s, size_t len) {
    w_before_value(w);
    w_put_string(w, s, len);
}

void arcjson_write_int(arcjson_writer *w, int64_t v) {
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%" PRId64, v);
    w_before_value(w);
    w_put(w, buf, (size_t)n);
}

/* w_put_double: %.*g with the given precision; non-finite values become null. */
static void w_put_double(arcjson_writer *w, double v, int precision) {
    char buf[40];
    w_before_value(w);
    if (!isfinite(v)) {
        w_put(w, "null", 4);
        return;
    }
    int n = snprintf(buf, sizeof(buf), "%.*g", precision, v);
    w_put(w, buf, (size_t)n);
}

void arcjson_write_number(arcjson_writer *w, double v) {
    w_put_double(w, v, 17);
}

void arcjson_write_float(arcjson_writer *w, float v) {
    w_put_double(w, v, 9);
}

void arcjson_write_bool(arcjson_writer *w, bool v) {
    w_before_value(w);
    w_put(w, v ? "true" : "false", v ? 4 : 5);
}

void arcjson_write_null(arcjson_writer *w) {
    w_before_value(w);
    w_put(w, "null", 4);
}

/* ---- Manifests ---------------------------------------------------------------------------- */

char *arcjson_read_file(const char *path, size_t max_len, size_t *len, const char **why) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *why = strerror(errno);
        return NULL;
    }
    char *buf = malloc(max_len + 2);
    size_t got = 0;
    *why = buf == NULL ? "out of memory" : NULL;
    while (buf != NULL && got <= max_len) {
        ssize_t n = read(fd, buf + got, max_len + 1 - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            *why = strerror(errno);
        }
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    if (*why == NULL && got > max_len) {
        *why = "larger than the size limit";
    }
    if (*why != NULL) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    *len = got;
    return buf;
}

bool arcjson_is_schema_id(const char *s, size_t len) {
    if (len == 0 || len > 64 || s[0] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) {
            return false;
        }
    }
    return true;
}

bool arcjson_is_schema_version(const char *s, size_t len) {
    if (len == 0 || len > 64 || s[0] < '0' || s[0] > '9') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '+')) {
            return false;
        }
    }
    return true;
}

/* version_id_cmp: one dot-separated semver identifier against another: numbers by value, numbers before words. */
static int version_id_cmp(const char *a, size_t a_len, const char *b, size_t b_len) {
    bool a_num = strspn(a, "0123456789") >= a_len;
    bool b_num = strspn(b, "0123456789") >= b_len;
    if (a_num != b_num) {
        return a_num ? -1 : 1;
    }
    if (a_num) {
        while (a_len > 1 && *a == '0') { /* leading zeros do not change the value */
            a++, a_len--;
        }
        while (b_len > 1 && *b == '0') {
            b++, b_len--;
        }
        if (a_len != b_len) {
            return a_len < b_len ? -1 : 1;
        }
    }
    size_t n = a_len < b_len ? a_len : b_len;
    int c = memcmp(a, b, n);
    return c != 0 ? c : (a_len > b_len) - (a_len < b_len);
}

/* version_list_cmp: dot-separated identifier lists a[0..a_len) and b[0..b_len); a prefix sorts first. */
static int version_list_cmp(const char *a, size_t a_len, const char *b, size_t b_len) {
    size_t i = 0, j = 0;
    while (i < a_len && j < b_len) {
        size_t m = i, n = j;
        while (m < a_len && a[m] != '.') {
            m++;
        }
        while (n < b_len && b[n] != '.') {
            n++;
        }
        int c = version_id_cmp(a + i, m - i, b + j, n - j);
        if (c != 0) {
            return c;
        }
        i = m + 1;
        j = n + 1;
    }
    return (i < a_len) - (j < b_len);
}

int arcjson_schema_version_cmp(const char *a, const char *b) {
    size_t a_end = strcspn(a, "+"), b_end = strcspn(b, "+");
    size_t a_main = strcspn(a, "-+"), b_main = strcspn(b, "-+");
    int c = version_list_cmp(a, a_main, b, b_main);
    if (c != 0) {
        return c;
    }
    bool a_pre = a_main < a_end, b_pre = b_main < b_end;
    if (a_pre != b_pre) {
        return a_pre ? -1 : 1; /* 1.0.0-rc.1 < 1.0.0 */
    }
    return a_pre ? version_list_cmp(a + a_main + 1, a_end - a_main - 1, b + b_main + 1, b_end - b_main - 1) : 0;
}
