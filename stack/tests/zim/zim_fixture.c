/*
 * zim_fixture.c — the test ZIM writer (see zim_fixture.h). Writes into one growing buffer.
 */
#include "zim_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "arczim.h"

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool oom;
} buf_t;

static void buf_put(buf_t *b, const void *data, size_t n) {
    if (b->oom) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) {
            cap *= 2;
        }
        uint8_t *grown = realloc(b->p, cap);
        if (grown == NULL) {
            b->oom = true;
            return;
        }
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void put8(buf_t *b, uint8_t v) {
    buf_put(b, &v, 1);
}
static void put16(buf_t *b, uint16_t v) {
    buf_put(b, &v, 2);
}
static void put32(buf_t *b, uint32_t v) {
    buf_put(b, &v, 4);
}
static void put64(buf_t *b, uint64_t v) {
    buf_put(b, &v, 8);
}
static void put_str(buf_t *b, const char *s) {
    buf_put(b, s, strlen(s) + 1);
}
static void set64(buf_t *b, size_t at, uint64_t v) {
    if (!b->oom) {
        memcpy(b->p + at, &v, 8);
    }
}

/* Sorting entries by (namespace, path) or (namespace, title). */
static const zimfx_entry *sort_base;
static bool sort_by_title;

static const char *sort_key(const zimfx_entry *e) {
    return sort_by_title && e->title && e->title[0] ? e->title : e->path;
}

static int cmp_idx(const void *a, const void *b) {
    const zimfx_entry *x = &sort_base[*(const uint32_t *)a], *y = &sort_base[*(const uint32_t *)b];
    if (x->ns != y->ns) {
        return (unsigned char)x->ns < (unsigned char)y->ns ? -1 : 1;
    }
    return strcmp(sort_key(x), sort_key(y));
}

/* find_entry: entry number of "<ns>/<path>" in the sorted order, or -1. */
static long find_entry(const zimfx_spec *s, const uint32_t *order, const char *key) {
    if (key == NULL || key[0] == '\0' || key[1] != '/') {
        return -1;
    }
    for (size_t i = 0; i < s->n_entries; i++) {
        const zimfx_entry *e = &s->entries[order[i]];
        if (e->ns == key[0] && strcmp(e->path, key + 2) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* mime_index: index of `m` in the list, adding it. */
static uint16_t mime_index(const char **list, size_t *n, const char *m) {
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(list[i], m) == 0) {
            return (uint16_t)i;
        }
    }
    list[*n] = m;
    return (uint16_t)(*n)++;
}

/* put_cluster: cluster `c` of the spec: info byte, then the offset table and blobs (compressed or not);
 * blob numbers of its entries go into blob_of. */
static bool put_cluster(buf_t *b, const zimfx_spec *s, unsigned c, uint32_t *blob_of, char *err, size_t errlen) {
    const zimfx_cluster *cl = &s->clusters[c];
    unsigned osz = cl->extended ? 8 : 4;
    size_t n = 0;
    for (size_t i = 0; i < s->n_entries; i++) {
        if (!s->entries[i].redirect && s->entries[i].cluster == c) {
            blob_of[i] = (uint32_t)n++;
        }
    }
    buf_t raw = {0};
    uint64_t off = (n + 1) * osz;
    for (size_t i = 0; i <= s->n_entries; i++) {
        if (i == s->n_entries || (!s->entries[i].redirect && s->entries[i].cluster == c)) {
            if (osz == 8) {
                put64(&raw, off);
            } else {
                put32(&raw, (uint32_t)off);
            }
            off += i < s->n_entries ? s->entries[i].size : 0;
        }
    }
    for (size_t i = 0; i < s->n_entries; i++) {
        if (!s->entries[i].redirect && s->entries[i].cluster == c) {
            buf_put(&raw, s->entries[i].data, s->entries[i].size);
        }
    }
    put8(b, (uint8_t)(cl->compression | (cl->extended ? ARCZIM_CLUSTER_EXTENDED : 0)));
    bool ok = !raw.oom;
    if (ok && cl->compression == ARCZIM_COMP_ZSTD) {
        /* Level 3 with zstd's content checksum on, so damage tests can count on it being caught. */
        size_t bound = ZSTD_compressBound(raw.len);
        uint8_t *z = malloc(bound);
        ZSTD_CCtx *cctx = ZSTD_createCCtx();
        size_t zn = 0;
        ok = z != NULL && cctx != NULL && !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3)) &&
             !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        if (ok) {
            zn = ZSTD_compress2(cctx, z, bound, raw.p, raw.len);
            ok = !ZSTD_isError(zn);
        }
        if (ok) {
            buf_put(b, z, zn);
        }
        ZSTD_freeCCtx(cctx);
        free(z);
    } else if (ok) {
        buf_put(b, raw.p, raw.len);
    }
    free(raw.p);
    if (!ok) {
        snprintf(err, errlen, "cluster %u: out of memory or zstd error", c);
    }
    return ok;
}

/* One build's working state. */
typedef struct {
    const zimfx_spec *s;
    uint32_t *order;   /* entry number -> spec index */
    uint32_t *rank;    /* spec index -> entry number */
    uint32_t *blob_of; /* spec index -> blob number in its cluster */
    uint16_t *mime_of; /* spec index -> MIME index */
    const char **mimes;
    size_t n_mimes;
    uint64_t *dirent_pos;
    uint64_t *cluster_pos;
    uint64_t pos[5]; /* mime list, path list, title list, cluster list, checksum */
    buf_t b;
    char *err;
    size_t errlen;
} fx_build;

/* fx_prepare: MIME indexes, path order and ranks; false on a bad cluster or a repeated path. */
static bool fx_prepare(fx_build *f) {
    const zimfx_spec *s = f->s;
    for (size_t i = 0; i < s->n_entries; i++) {
        f->order[i] = (uint32_t)i;
        if (s->entries[i].redirect) {
            continue;
        }
        if (s->entries[i].cluster >= s->n_clusters) {
            snprintf(f->err, f->errlen, "entry %zu: no cluster %u", i, s->entries[i].cluster);
            return false;
        }
        f->mime_of[i] = mime_index(f->mimes, &f->n_mimes, s->entries[i].mime);
    }
    sort_base = s->entries;
    sort_by_title = false;
    qsort(f->order, s->n_entries, sizeof(*f->order), cmp_idx);
    for (size_t i = 0; i < s->n_entries; i++) {
        f->rank[f->order[i]] = (uint32_t)i;
        if (i > 0 && cmp_idx(&f->order[i - 1], &f->order[i]) == 0) {
            const zimfx_entry *e = &s->entries[f->order[i]];
            snprintf(f->err, f->errlen, "repeated path %c/%s", e->ns, e->path);
            return false;
        }
    }
    return true;
}

/* fx_put_dirents: the dirents in path order. */
static bool fx_put_dirents(fx_build *f) {
    for (size_t i = 0; i < f->s->n_entries; i++) {
        const zimfx_entry *e = &f->s->entries[f->order[i]];
        f->dirent_pos[i] = f->b.len;
        if (e->redirect) {
            long t = find_entry(f->s, f->order, e->redirect);
            if (t < 0) {
                snprintf(f->err, f->errlen, "%c/%s: unknown redirect target %s", e->ns, e->path, e->redirect);
                return false;
            }
            put16(&f->b, ARCZIM_MIME_REDIRECT);
        } else {
            put16(&f->b, f->mime_of[f->order[i]]);
        }
        put8(&f->b, 0);
        put8(&f->b, (uint8_t)e->ns);
        put32(&f->b, 0);
        if (e->redirect) {
            put32(&f->b, (uint32_t)find_entry(f->s, f->order, e->redirect));
        } else {
            put32(&f->b, e->cluster);
            put32(&f->b, f->blob_of[f->order[i]]);
        }
        put_str(&f->b, e->path);
        put_str(&f->b, e->title && strcmp(e->title, e->path) != 0 ? e->title : "");
    }
    return true;
}

/* fx_put_lists: path pointer list, title pointer list (if asked), cluster pointer list, checksum. */
static bool fx_put_lists(fx_build *f) {
    size_t n = f->s->n_entries;
    f->pos[1] = f->b.len;
    for (size_t i = 0; i < n; i++) {
        put64(&f->b, f->dirent_pos[i]);
    }
    f->pos[2] = ARCZIM_NO_TITLE_PTR;
    if (f->s->title_list) {
        uint32_t *by_title = malloc((n + 1) * sizeof(*by_title));
        if (by_title == NULL) {
            return false;
        }
        memcpy(by_title, f->order, n * sizeof(*by_title));
        sort_by_title = true;
        qsort(by_title, n, sizeof(*by_title), cmp_idx);
        f->pos[2] = f->b.len;
        for (size_t i = 0; i < n; i++) {
            put32(&f->b, f->rank[by_title[i]]);
        }
        free(by_title);
    }
    f->pos[3] = f->b.len;
    for (size_t c = 0; c < f->s->n_clusters; c++) {
        put64(&f->b, f->cluster_pos[c]);
    }
    f->pos[4] = f->b.len;
    static const uint8_t md5_zero[16];
    buf_put(&f->b, md5_zero, sizeof(md5_zero));
    return true;
}

/* fx_header: fill in the 80-byte header; false if the main page is unknown. */
static bool fx_header(fx_build *f) {
    long main_page = f->s->main_page ? find_entry(f->s, f->order, f->s->main_page) : -1;
    if (f->s->main_page && main_page < 0) {
        snprintf(f->err, f->errlen, "unknown main page %s", f->s->main_page);
        return false;
    }
    uint8_t *h = f->b.p;
    uint32_t magic = ARCZIM_MAGIC;
    memcpy(h, &magic, 4);
    memcpy(h + 4, &f->s->major, 2);
    memcpy(h + 6, &f->s->minor, 2);
    for (int i = 0; i < 16; i++) {
        h[8 + i] = (uint8_t)(0xA0 + i);
    }
    uint32_t counts[2] = {(uint32_t)f->s->n_entries, (uint32_t)f->s->n_clusters};
    memcpy(h + 24, counts, 8);
    set64(&f->b, 32, f->pos[1]);
    set64(&f->b, 40, f->pos[2]);
    set64(&f->b, 48, f->pos[3]);
    set64(&f->b, 56, f->pos[0]);
    uint32_t pages[2] = {main_page < 0 ? ARCZIM_NO_ENTRY : (uint32_t)main_page, ARCZIM_NO_ENTRY};
    memcpy(h + 64, pages, 8);
    set64(&f->b, 72, f->pos[4]);
    return true;
}

bool zimfx_build(const zimfx_spec *s, uint8_t **out, size_t *len, zimfx_layout *lay, char *err, size_t errlen) {
    size_t n = s->n_entries + 1;
    fx_build f = {
        .s = s,
        .order = malloc(n * sizeof(uint32_t)),
        .rank = malloc(n * sizeof(uint32_t)),
        .blob_of = calloc(n, sizeof(uint32_t)),
        .mime_of = calloc(n, sizeof(uint16_t)),
        .mimes = calloc(n, sizeof(const char *)),
        .dirent_pos = calloc(n, sizeof(uint64_t)),
        .cluster_pos = calloc(s->n_clusters + 1, sizeof(uint64_t)),
        .err = err,
        .errlen = errlen,
    };
    snprintf(err, errlen, "out of memory");
    bool ok = f.order && f.rank && f.blob_of && f.mime_of && f.mimes && f.dirent_pos && f.cluster_pos && fx_prepare(&f);
    static const uint8_t header[ARCZIM_HEADER_SIZE]; /* filled in by fx_header */
    buf_put(&f.b, header, sizeof(header));
    f.pos[0] = f.b.len;
    for (size_t i = 0; ok && i < f.n_mimes; i++) {
        put_str(&f.b, f.mimes[i]);
    }
    put8(&f.b, 0);
    for (unsigned c = 0; ok && c < s->n_clusters; c++) {
        f.cluster_pos[c] = f.b.len;
        ok = put_cluster(&f.b, s, c, f.blob_of, err, errlen);
    }
    ok = ok && fx_put_dirents(&f) && fx_put_lists(&f) && !f.b.oom && fx_header(&f);
    if (ok) {
        *out = f.b.p;
        *len = f.b.len;
        f.b.p = NULL;
        if (lay) {
            *lay =
                (zimfx_layout){f.pos[0], f.pos[1], f.pos[2], f.pos[3], f.pos[4], f.dirent_pos, f.rank, f.cluster_pos};
            f.dirent_pos = NULL;
            f.rank = NULL;
            f.cluster_pos = NULL;
        }
    }
    free(f.b.p);
    free(f.order);
    free(f.rank);
    free(f.blob_of);
    free(f.mime_of);
    free(f.mimes);
    free(f.dirent_pos);
    free(f.cluster_pos);
    return ok;
}

void zimfx_layout_free(zimfx_layout *lay) {
    free(lay->dirent_pos);
    free(lay->entry_of);
    free(lay->cluster_pos);
    memset(lay, 0, sizeof(*lay));
}

bool zimfx_write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(buf, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}
