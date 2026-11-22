/*
 * arczim.c — the ZIM reader (see arczim.h; format as we read it in docs/formats/zim.md).
 *
 * The file is mapped whole and read in place. arczim_open() checks the header, the MIME list and the
 * extents of the two pointer lists; every other structure (dirent, cluster, blob offset table) is
 * checked against the mapping when it is read. zstd clusters are decompressed with the streaming API
 * (which honours ZSTD_d_windowLogMax) into an LRU cache of reusable buffers.
 */
#include "arczim.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#define FIRST_OUT_BYTES (1u << 20) /* first buffer for a zstd cluster of unknown size */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t size; /* decompressed bytes */
    uint32_t cluster;
    bool valid;
    uint64_t used; /* LRU tick */
} cache_slot;

struct arczim {
    const uint8_t *map;
    uint64_t size;
    uint64_t data_end; /* checksumPos: nothing we read lies at or past it */
    arczim_header h;
    const char **mimes;
    uint16_t n_mimes;
    cache_slot slots[ARCZIM_CACHE_SLOTS_MAX];
    unsigned n_slots;
    uint64_t tick;
    size_t cluster_max;
    ZSTD_DCtx *dctx;
    char err[256];
};

/* A cluster's blob offset table, located (in the mapping or a cache slot). */
typedef struct {
    const uint8_t *data; /* first byte after the info byte, decompressed */
    uint64_t avail;      /* bytes readable from data */
    unsigned offsize;    /* 4 or 8 */
    uint64_t n_blobs;
} blob_table;

/* ---- Errors and little helpers --------------------------------------------------------------- */

static _Thread_local char open_why[512];

/* zim_fail: format the error of `z` and return false. */
__attribute__((format(printf, 2, 3))) static bool zim_fail(arczim *z, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(z->err, sizeof(z->err), fmt, ap);
    va_end(ap);
    return false;
}

const char *arczim_error(const arczim *z) {
    return z->err;
}

static uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* rd_off: one blob offset of `size` bytes (4 or 8). */
static uint64_t rd_off(const uint8_t *p, unsigned size) {
    return size == 8 ? rd64(p) : rd32(p);
}

/* in_data: [off, off+len) lies before the checksum. Overflow-safe. */
static bool in_data(const arczim *z, uint64_t off, uint64_t len) {
    return off <= z->data_end && len <= z->data_end - off;
}

/* bounded_string: length of the NUL-terminated string at `off` (at most ARCZIM_STRING_MAX bytes, the
 * NUL before data_end); false if it has none. */
static bool bounded_string(const arczim *z, uint64_t off, size_t *len) {
    if (off >= z->data_end) {
        return false;
    }
    uint64_t room = z->data_end - off;
    size_t scan = room > ARCZIM_STRING_MAX + 1u ? ARCZIM_STRING_MAX + 1u : (size_t)room;
    const uint8_t *nul = memchr(z->map + off, 0, scan);
    if (nul == NULL) {
        return false;
    }
    *len = (size_t)(nul - (z->map + off));
    return true;
}

/* ---- Opening ---------------------------------------------------------------------------------- */

/* open_fail: set the open error and return NULL (closing what was opened). */
__attribute__((format(printf, 3, 4))) static arczim *open_fail(arczim *z, const char **why, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(open_why, sizeof(open_why), fmt, ap);
    va_end(ap);
    *why = open_why;
    arczim_close(z);
    return NULL;
}

/* read_header: the 80 header bytes into z->h. */
static void read_header(arczim *z) {
    const uint8_t *p = z->map;
    z->h.major = rd16(p + 4);
    z->h.minor = rd16(p + 6);
    memcpy(z->h.uuid, p + 8, 16);
    z->h.entry_count = rd32(p + 24);
    z->h.cluster_count = rd32(p + 28);
    z->h.path_ptr_pos = rd64(p + 32);
    z->h.title_ptr_pos = rd64(p + 40);
    z->h.cluster_ptr_pos = rd64(p + 48);
    z->h.mime_list_pos = rd64(p + 56);
    z->h.main_page = rd32(p + 64);
    z->h.layout_page = rd32(p + 68);
    z->h.checksum_pos = rd64(p + 72);
}

/* read_mimes: the MIME list (NUL-terminated strings ending with an empty one) into z->mimes. */
static bool read_mimes(arczim *z) {
    uint64_t off = z->h.mime_list_pos;
    size_t cap = 0;
    for (;;) {
        size_t len;
        if (!bounded_string(z, off, &len)) {
            return zim_fail(z, "MIME list: string at %llu not terminated", (unsigned long long)off);
        }
        if (len == 0) {
            return true;
        }
        if (z->n_mimes == ARCZIM_MIME_MAX) {
            return zim_fail(z, "MIME list: more than %u types", ARCZIM_MIME_MAX);
        }
        if (z->n_mimes == cap) {
            cap = cap ? cap * 2 : 16;
            const char **grown = realloc(z->mimes, cap * sizeof(*grown));
            if (grown == NULL) {
                return zim_fail(z, "out of memory");
            }
            z->mimes = grown;
        }
        z->mimes[z->n_mimes++] = (const char *)z->map + off;
        off += len + 1;
    }
}

/* check_header: the header fields against the file. */
static bool check_header(arczim *z) {
    const arczim_header *h = &z->h;
    if (rd32(z->map) != ARCZIM_MAGIC) {
        return zim_fail(z, "not a ZIM file (bad magic number)");
    }
    if (h->major != 5 && h->major != 6) {
        return zim_fail(z, "ZIM major version %u not supported (5 and 6 are)", h->major);
    }
    if (h->checksum_pos > z->size || z->size - h->checksum_pos < 16) {
        return zim_fail(z, "checksum position %llu outside the file", (unsigned long long)h->checksum_pos);
    }
    z->data_end = h->checksum_pos;
    if (h->mime_list_pos < ARCZIM_HEADER_SIZE || h->mime_list_pos >= z->data_end) {
        return zim_fail(z, "MIME list position %llu outside the file", (unsigned long long)h->mime_list_pos);
    }
    if (!in_data(z, h->path_ptr_pos, (uint64_t)h->entry_count * 8)) {
        return zim_fail(z, "path pointer list (%u entries at %llu) outside the file", h->entry_count,
                        (unsigned long long)h->path_ptr_pos);
    }
    if (!in_data(z, h->cluster_ptr_pos, (uint64_t)h->cluster_count * 8)) {
        return zim_fail(z, "cluster pointer list (%u clusters at %llu) outside the file", h->cluster_count,
                        (unsigned long long)h->cluster_ptr_pos);
    }
    if (h->main_page != ARCZIM_NO_ENTRY && h->main_page >= h->entry_count) {
        return zim_fail(z, "main page %u is not an entry", h->main_page);
    }
    return read_mimes(z);
}

arczim *arczim_open(const char *path, const char **why) {
    arczim *z = calloc(1, sizeof(*z));
    if (z == NULL) {
        return open_fail(NULL, why, "out of memory");
    }
    z->n_slots = ARCZIM_CACHE_SLOTS_DEFAULT;
    z->cluster_max = ARCZIM_CLUSTER_MAX_DEFAULT;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return open_fail(z, why, "%s: %s", path, strerror(errno));
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return open_fail(z, why, "%s: not a regular file", path);
    }
    if ((uint64_t)st.st_size < ARCZIM_HEADER_SIZE) {
        close(fd);
        return open_fail(z, why, "%s: %lld bytes, shorter than a ZIM header", path, (long long)st.st_size);
    }
    z->size = (uint64_t)st.st_size;
    void *m = mmap(NULL, (size_t)z->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        return open_fail(z, why, "%s: mmap: %s", path, strerror(errno));
    }
    z->map = m;
    read_header(z);
    if (!check_header(z)) {
        return open_fail(z, why, "%s: %s", path, z->err);
    }
    return z;
}

/* drop_cache: free every cached cluster buffer. */
static void drop_cache(arczim *z) {
    for (unsigned i = 0; i < ARCZIM_CACHE_SLOTS_MAX; i++) {
        free(z->slots[i].buf);
        memset(&z->slots[i], 0, sizeof(z->slots[i]));
    }
}

void arczim_close(arczim *z) {
    if (z == NULL) {
        return;
    }
    drop_cache(z);
    ZSTD_freeDCtx(z->dctx);
    free(z->mimes);
    if (z->map != NULL) {
        munmap((void *)z->map, (size_t)z->size);
    }
    free(z);
}

bool arczim_set_cache_slots(arczim *z, unsigned slots) {
    if (slots < 1 || slots > ARCZIM_CACHE_SLOTS_MAX) {
        return zim_fail(z, "cache slots must be 1..%u", ARCZIM_CACHE_SLOTS_MAX);
    }
    drop_cache(z);
    z->n_slots = slots;
    return true;
}

bool arczim_set_cluster_max(arczim *z, size_t bytes) {
    if (bytes < 64) {
        return zim_fail(z, "cluster limit must be at least 64 bytes");
    }
    drop_cache(z);
    z->cluster_max = bytes;
    return true;
}

const arczim_header *arczim_get_header(const arczim *z) {
    return &z->h;
}

uint64_t arczim_file_size(const arczim *z) {
    return z->size;
}

uint32_t arczim_entry_count(const arczim *z) {
    return z->h.entry_count;
}

uint32_t arczim_cluster_count(const arczim *z) {
    return z->h.cluster_count;
}

uint16_t arczim_mime_count(const arczim *z) {
    return z->n_mimes;
}

const char *arczim_mime(const arczim *z, uint16_t i) {
    return i < z->n_mimes ? z->mimes[i] : NULL;
}

/* ---- Directory entries ------------------------------------------------------------------------- */

/* parse_dirent: the dirent at file offset `off` (entry number `index`). */
static bool parse_dirent(arczim *z, uint32_t index, uint64_t off, arczim_entry *e) {
    if (!in_data(z, off, 8)) {
        return zim_fail(z, "entry %u: dirent at %llu outside the file", index, (unsigned long long)off);
    }
    const uint8_t *p = z->map + off;
    memset(e, 0, sizeof(*e));
    e->index = index;
    uint16_t mime = rd16(p);
    uint8_t param_len = p[2];
    e->ns = (char)p[3];
    uint64_t strings;
    if (mime == ARCZIM_MIME_REDIRECT) {
        if (!in_data(z, off, 12)) {
            return zim_fail(z, "entry %u: redirect dirent cut short", index);
        }
        e->kind = ARCZIM_ENTRY_REDIRECT;
        e->redirect = rd32(p + 8);
        if (e->redirect >= z->h.entry_count) {
            return zim_fail(z, "entry %u: redirect to entry %u of %u", index, e->redirect, z->h.entry_count);
        }
        strings = off + 12;
    } else if (mime == ARCZIM_MIME_LINKTARGET || mime == ARCZIM_MIME_DELETED) {
        e->kind = ARCZIM_ENTRY_OBSOLETE;
        strings = off + 8;
    } else {
        if (!in_data(z, off, 16)) {
            return zim_fail(z, "entry %u: content dirent cut short", index);
        }
        e->kind = ARCZIM_ENTRY_CONTENT;
        e->mime = mime;
        e->cluster = rd32(p + 8);
        e->blob = rd32(p + 12);
        strings = off + 16;
        if (mime >= z->n_mimes) {
            return zim_fail(z, "entry %u: MIME number %u of %u", index, mime, z->n_mimes);
        }
        if (e->cluster >= z->h.cluster_count) {
            return zim_fail(z, "entry %u: cluster %u of %u", index, e->cluster, z->h.cluster_count);
        }
    }
    if (!bounded_string(z, strings, &e->path_length)) {
        return zim_fail(z, "entry %u: path not terminated", index);
    }
    uint64_t title_off = strings + e->path_length + 1;
    if (!bounded_string(z, title_off, &e->title_length)) {
        return zim_fail(z, "entry %u: title not terminated", index);
    }
    if (!in_data(z, title_off + e->title_length + 1, param_len)) {
        return zim_fail(z, "entry %u: extra parameters cut short", index);
    }
    e->path = (const char *)z->map + strings;
    e->title = (const char *)z->map + title_off;
    if (e->title_length == 0) {
        e->title = e->path;
        e->title_length = e->path_length;
    }
    return true;
}

bool arczim_entry_get(arczim *z, uint32_t index, arczim_entry *out) {
    if (index >= z->h.entry_count) {
        return zim_fail(z, "entry %u of %u", index, z->h.entry_count);
    }
    return parse_dirent(z, index, rd64(z->map + z->h.path_ptr_pos + (uint64_t)index * 8), out);
}

/* key_cmp: order of entry `e` against (ns, path[0..len)): bytes, namespace first. */
static int key_cmp(const arczim_entry *e, char ns, const char *path, size_t len) {
    if ((unsigned char)e->ns != (unsigned char)ns) {
        return (unsigned char)e->ns < (unsigned char)ns ? -1 : 1;
    }
    size_t n = e->path_length < len ? e->path_length : len;
    int c = memcmp(e->path, path, n);
    if (c != 0) {
        return c;
    }
    return e->path_length < len ? -1 : e->path_length > len;
}

bool arczim_lower_bound(arczim *z, char ns, const char *path, size_t len, uint32_t *index) {
    uint32_t lo = 0, hi = z->h.entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        arczim_entry e;
        if (!arczim_entry_get(z, mid, &e)) {
            return false;
        }
        if (key_cmp(&e, ns, path, len) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *index = lo;
    return true;
}

bool arczim_find(arczim *z, char ns, const char *path, size_t len, uint32_t *index) {
    uint32_t at;
    arczim_entry e;
    if (!arczim_lower_bound(z, ns, path, len, &at)) {
        return false;
    }
    if (at == z->h.entry_count) {
        return zim_fail(z, "not found");
    }
    if (!arczim_entry_get(z, at, &e)) {
        return false;
    }
    if (key_cmp(&e, ns, path, len) != 0) {
        return zim_fail(z, "not found");
    }
    *index = at;
    return true;
}

bool arczim_resolve(arczim *z, uint32_t index, arczim_entry *out) {
    for (unsigned hop = 0; hop <= ARCZIM_REDIRECT_HOPS_MAX; hop++) {
        if (!arczim_entry_get(z, index, out)) {
            return false;
        }
        if (out->kind == ARCZIM_ENTRY_CONTENT) {
            return true;
        }
        if (out->kind == ARCZIM_ENTRY_OBSOLETE) {
            return zim_fail(z, "entry %u: obsolete entry has no content", index);
        }
        index = out->redirect;
    }
    return zim_fail(z, "entry %u: more than %u redirects (a loop?)", out->index, ARCZIM_REDIRECT_HOPS_MAX);
}

/* ---- Clusters and blobs ------------------------------------------------------------------------ */

bool arczim_cluster_get(arczim *z, uint32_t n, arczim_cluster *out) {
    if (n >= z->h.cluster_count) {
        return zim_fail(z, "cluster %u of %u", n, z->h.cluster_count);
    }
    uint64_t off = rd64(z->map + z->h.cluster_ptr_pos + (uint64_t)n * 8);
    if (!in_data(z, off, 1)) {
        return zim_fail(z, "cluster %u: at %llu, outside the file", n, (unsigned long long)off);
    }
    uint8_t info = z->map[off];
    out->offset = off;
    out->compression = info & 0x0F;
    out->extended = (info & ARCZIM_CLUSTER_EXTENDED) != 0;
    if ((info & 0xE0) != 0) {
        return zim_fail(z, "cluster %u: unknown flags in info byte 0x%02x", n, info);
    }
    if (out->extended && z->h.major < 6) {
        return zim_fail(z, "cluster %u: extended cluster in a version 5 file", n);
    }
    return true;
}

/* zstd_cluster: decompress zstd cluster `n` (info byte at `off`) into a cache slot. */
static bool zstd_cluster(arczim *z, uint32_t n, uint64_t off, cache_slot **out) {
    cache_slot *slot = NULL;
    for (unsigned i = 0; i < z->n_slots; i++) {
        cache_slot *s = &z->slots[i];
        if (s->valid && s->cluster == n) {
            s->used = ++z->tick;
            *out = s;
            return true;
        }
        if (slot == NULL || !s->valid || (slot->valid && s->used < slot->used)) {
            slot = s; /* an empty slot, else the least recently used */
        }
    }
    slot->valid = false;
    if (z->dctx == NULL) {
        z->dctx = ZSTD_createDCtx();
        if (z->dctx == NULL ||
            ZSTD_isError(ZSTD_DCtx_setParameter(z->dctx, ZSTD_d_windowLogMax, ARCZIM_ZSTD_WINDOW_LOG_MAX))) {
            return zim_fail(z, "out of memory (zstd)");
        }
    }
    ZSTD_DCtx_reset(z->dctx, ZSTD_reset_session_only);
    ZSTD_inBuffer in = {z->map + off + 1, (size_t)(z->data_end - off - 1), 0};
    unsigned long long known = ZSTD_getFrameContentSize(in.src, in.size);
    if (known == ZSTD_CONTENTSIZE_ERROR) {
        return zim_fail(z, "cluster %u: not a zstd frame", n);
    }
    if (known != ZSTD_CONTENTSIZE_UNKNOWN && known > z->cluster_max) {
        return zim_fail(z, "cluster %u: %llu bytes, over the %zu-byte limit", n, known, z->cluster_max);
    }
    size_t want = known != ZSTD_CONTENTSIZE_UNKNOWN ? (size_t)known + 1 : FIRST_OUT_BYTES;
    if (want > z->cluster_max) {
        want = z->cluster_max;
    }
    size_t pos = 0;
    for (;;) {
        if (slot->cap < want) {
            uint8_t *grown = realloc(slot->buf, want);
            if (grown == NULL) {
                return zim_fail(z, "cluster %u: out of memory", n);
            }
            slot->buf = grown;
            slot->cap = want;
        }
        ZSTD_outBuffer o = {slot->buf, want, pos};
        size_t ret = ZSTD_decompressStream(z->dctx, &o, &in);
        pos = o.pos;
        if (ZSTD_isError(ret)) {
            return zim_fail(z, "cluster %u: zstd: %s", n, ZSTD_getErrorName(ret));
        }
        if (ret == 0) {
            break; /* frame complete */
        }
        if (o.pos < o.size) {
            if (in.pos == in.size) {
                return zim_fail(z, "cluster %u: zstd frame cut short", n);
            }
            continue;
        }
        if (want == z->cluster_max) {
            return zim_fail(z, "cluster %u: over the %zu-byte limit", n, z->cluster_max);
        }
        want = want > z->cluster_max / 2 ? z->cluster_max : want * 2;
    }
    slot->size = pos;
    slot->cluster = n;
    slot->valid = true;
    slot->used = ++z->tick;
    *out = slot;
    return true;
}

/* blob_table_get: locate cluster `n`'s blob offset table (decompressing it if needed). */
static bool blob_table_get(arczim *z, uint32_t n, blob_table *t) {
    arczim_cluster c;
    if (!arczim_cluster_get(z, n, &c)) {
        return false;
    }
    t->offsize = c.extended ? 8 : 4;
    switch (c.compression) {
    case ARCZIM_COMP_NONE_OLD:
    case ARCZIM_COMP_NONE:
        t->data = z->map + c.offset + 1;
        t->avail = z->data_end - c.offset - 1;
        break;
    case ARCZIM_COMP_ZSTD: {
        cache_slot *s;
        if (!zstd_cluster(z, n, c.offset, &s)) {
            return false;
        }
        t->data = s->buf;
        t->avail = s->size;
        break;
    }
    case ARCZIM_COMP_XZ:
        return zim_fail(z, "cluster %u: xz compression is not supported (ADR-0022)", n);
    case ARCZIM_COMP_ZLIB:
    case ARCZIM_COMP_BZIP2:
        return zim_fail(z, "cluster %u: compression %u was removed from the ZIM format", n, c.compression);
    default:
        return zim_fail(z, "cluster %u: unknown compression %u", n, c.compression);
    }
    if (t->avail < t->offsize) {
        return zim_fail(z, "cluster %u: no blob offset table", n);
    }
    uint64_t first = rd_off(t->data, t->offsize);
    if (first < t->offsize || first % t->offsize != 0 || first > t->avail) {
        return zim_fail(z, "cluster %u: bad first blob offset %llu", n, (unsigned long long)first);
    }
    t->n_blobs = first / t->offsize - 1;
    return true;
}

/* blob_range: blob `b`'s [start, start+size) in table `t`, checked. */
static bool blob_range(arczim *z, uint32_t n, const blob_table *t, uint64_t b, uint64_t *start, uint64_t *size) {
    if (b >= t->n_blobs) {
        return zim_fail(z, "cluster %u: blob %llu of %llu", n, (unsigned long long)b, (unsigned long long)t->n_blobs);
    }
    uint64_t first = (t->n_blobs + 1) * t->offsize;
    uint64_t s = rd_off(t->data + b * t->offsize, t->offsize);
    uint64_t e = rd_off(t->data + (b + 1) * t->offsize, t->offsize);
    if (s < first || s > e || e > t->avail) {
        return zim_fail(z, "cluster %u: blob %llu has bad offsets %llu..%llu", n, (unsigned long long)b,
                        (unsigned long long)s, (unsigned long long)e);
    }
    *start = s;
    *size = e - s;
    return true;
}

bool arczim_blob(arczim *z, uint32_t cluster, uint32_t blob, const char **data, uint64_t *size) {
    blob_table t;
    uint64_t start;
    if (!blob_table_get(z, cluster, &t) || !blob_range(z, cluster, &t, blob, &start, size)) {
        return false;
    }
    *data = (const char *)t.data + start;
    return true;
}

bool arczim_content(arczim *z, const arczim_entry *e, const char **data, uint64_t *size) {
    if (e->kind != ARCZIM_ENTRY_CONTENT) {
        return zim_fail(z, "entry %u: not a content entry", e->index);
    }
    return arczim_blob(z, e->cluster, e->blob, data, size);
}

bool arczim_metadata(arczim *z, const char *key, const char **value, uint64_t *size) {
    uint32_t index;
    arczim_entry e;
    return arczim_find(z, 'M', key, strlen(key), &index) && arczim_resolve(z, index, &e) &&
           arczim_content(z, &e, value, size);
}

/* ---- Whole-file check -------------------------------------------------------------------------- */

/* Content entries grouped by cluster: blob number and MIME of each. */
typedef struct {
    uint32_t blob;
    uint16_t mime;
} blob_ref;

/* LRU cache simulation for the path-order locality numbers. */
typedef struct {
    uint32_t cluster[64];
    unsigned n;
} lru_sim;

/* lru_touch: access `c` in a cache of `cap` clusters; true on a miss. */
static bool lru_touch(lru_sim *l, unsigned cap, uint32_t c) {
    unsigned i = 0;
    while (i < l->n && l->cluster[i] != c) {
        i++;
    }
    bool miss = i == l->n;
    if (miss) {
        i = l->n < cap ? l->n++ : cap - 1;
    }
    memmove(&l->cluster[1], &l->cluster[0], i * sizeof(l->cluster[0]));
    l->cluster[0] = c;
    return miss;
}

/* is_html: a text/html MIME type, with or without parameters. */
static bool is_html(const char *mime) {
    return strncmp(mime, "text/html", 9) == 0 && (mime[9] == '\0' || mime[9] == ';');
}

/* check_entries: pass 1 over all dirents: order, kinds, redirects, counts per cluster, locality. */
static bool check_entries(arczim *z, arczim_stats *st, uint64_t *per_cluster) {
    lru_sim lru[ARCZIM_LRU_SIZES] = {0};
    arczim_entry prev = {0};
    for (uint32_t i = 0; i < z->h.entry_count; i++) {
        arczim_entry e;
        if (!arczim_entry_get(z, i, &e)) {
            return false;
        }
        if (i > 0 && key_cmp(&prev, e.ns, e.path, e.path_length) >= 0) {
            return zim_fail(z, "entries %u and %u are not in path order (or repeat a path)", i - 1, i);
        }
        prev = e;
        st->per_ns[(unsigned char)e.ns < 128 ? (unsigned char)e.ns : 0]++;
        if (e.kind == ARCZIM_ENTRY_REDIRECT) {
            arczim_entry target;
            if (!arczim_resolve(z, i, &target)) {
                return false;
            }
            st->redirects++;
            continue;
        }
        if (e.kind == ARCZIM_ENTRY_OBSOLETE) {
            st->obsolete++;
            continue;
        }
        st->content++;
        st->mime[e.mime].entries++;
        per_cluster[e.cluster + 1]++;
        if ((e.ns == 'C' || e.ns == 'A') && is_html(z->mimes[e.mime])) {
            st->html_entries++;
            for (unsigned k = 0; k < ARCZIM_LRU_SIZES; k++) {
                st->html_lru_misses[k] += lru_touch(&lru[k], arczim_lru_sizes[k], e.cluster);
            }
        }
    }
    return true;
}

/* group_refs: pass 2 over the dirents: each content entry's (blob, MIME), grouped by cluster. */
static bool group_refs(arczim *z, uint64_t *cursor, const uint64_t *start, blob_ref *refs) {
    for (uint32_t i = 0; i < z->h.entry_count; i++) {
        arczim_entry e;
        if (!arczim_entry_get(z, i, &e)) {
            return false;
        }
        if (e.kind == ARCZIM_ENTRY_CONTENT) {
            if (cursor[e.cluster] == start[e.cluster + 1]) {
                return zim_fail(z, "entry %u: the file changed while it was checked", i); /* pass 1 counted less */
            }
            refs[cursor[e.cluster]++] = (blob_ref){e.blob, e.mime};
        }
    }
    return true;
}

/* check_cluster: cluster `n` whole: its offset table, and the blobs refs[0..count) point to. */
static bool check_cluster(arczim *z, uint32_t n, const blob_ref *refs, uint64_t count, arczim_stats *st) {
    arczim_cluster c;
    blob_table t;
    if (!arczim_cluster_get(z, n, &c) || !blob_table_get(z, n, &t)) {
        return false;
    }
    st->clusters_by_comp[c.compression]++;
    st->clusters_extended += c.extended;
    uint64_t prev = (t.n_blobs + 1) * t.offsize;
    for (uint64_t b = 0; b <= t.n_blobs; b++) {
        uint64_t o = rd_off(t.data + b * t.offsize, t.offsize);
        if (o < prev || o > t.avail) {
            return zim_fail(z, "cluster %u: blob offset %llu out of order or past the end", n, (unsigned long long)b);
        }
        prev = o;
    }
    st->cluster_bytes += prev;
    if (prev > st->cluster_max_bytes) {
        st->cluster_max_bytes = prev;
    }
    st->blobs += t.n_blobs;
    uint8_t *seen = calloc(t.n_blobs ? t.n_blobs : 1, 1);
    if (seen == NULL) {
        return zim_fail(z, "out of memory");
    }
    for (uint64_t r = 0; r < count; r++) {
        uint64_t start, size;
        if (!blob_range(z, n, &t, refs[r].blob, &start, &size)) {
            free(seen);
            return false;
        }
        st->mime[refs[r].mime].bytes += size;
        st->blobs_shared += seen[refs[r].blob];
        seen[refs[r].blob] = 1;
    }
    for (uint64_t b = 0; b < t.n_blobs; b++) {
        st->blobs_unreferenced += !seen[b];
    }
    free(seen);
    return true;
}

bool arczim_check(arczim *z, arczim_stats *st) {
    memset(st, 0, sizeof(*st));
    uint32_t nc = z->h.cluster_count;
    st->mime = calloc(z->n_mimes ? z->n_mimes : 1, sizeof(*st->mime));
    uint64_t *per_cluster = calloc((size_t)nc + 1, sizeof(*per_cluster)); /* counts, then start indexes */
    uint64_t *cursor = malloc(((size_t)nc + 1) * sizeof(*cursor));
    blob_ref *refs = NULL;
    bool ok = st->mime != NULL && per_cluster != NULL && cursor != NULL;
    if (!ok) {
        zim_fail(z, "out of memory");
    }
    /* Cluster order reads each cluster once: one cache slot is enough and bounds memory by the largest cluster. */
    unsigned slots = z->n_slots;
    drop_cache(z);
    z->n_slots = 1;
    ok = ok && check_entries(z, st, per_cluster);
    if (ok) {
        for (uint32_t c = 0; c < nc; c++) {
            per_cluster[c + 1] += per_cluster[c];
        }
        memcpy(cursor, per_cluster, ((size_t)nc + 1) * sizeof(*cursor));
        refs = malloc((st->content ? st->content : 1) * sizeof(*refs));
        ok = refs != NULL || zim_fail(z, "out of memory");
    }
    ok = ok && group_refs(z, cursor, per_cluster, refs);
    for (uint32_t c = 0; ok && c < nc; c++) {
        ok = cursor[c] == per_cluster[c + 1] || zim_fail(z, "cluster %u: the file changed while it was checked", c);
    }
    for (uint32_t c = 0; ok && c < nc; c++) {
        ok = check_cluster(z, c, refs + per_cluster[c], per_cluster[c + 1] - per_cluster[c], st);
    }
    drop_cache(z);
    z->n_slots = slots;
    free(refs);
    free(cursor);
    free(per_cluster);
    return ok;
}

void arczim_stats_free(arczim_stats *st) {
    free(st->mime);
    st->mime = NULL;
}
