/*
 * zim_fixture — write a test ZIM from a JSON description (M3-03a). Test-only (zim_fixture.h).
 *
 *   zim_fixture SPEC.json OUT.zim
 *
 * SPEC: {"major": 6, "minor": 3, "title_list": false, "main_page": "C/Main",
 *        "clusters": [{"compression": "zstd" | "none" | "xz" | <code>, "extended": false}, ...],
 *        "entries": [{"ns": "C", "path": "A", "title": "...", "mime": "text/html", "text": "...", "cluster": 0},
 *                    {"ns": "C", "path": "B", "redirect": "C/A"}, ...],
 *        "generate": {"count": N, "clusters": K, "bytes": S}}
 * "generate" adds N text/html entries C/gen/0000000... of about S bytes each, spread over K more zstd
 * clusters out of path order (entry i goes to cluster i * 7919 mod K), like a real ZIM's poor locality.
 * An "xz" cluster is written with its info byte only marked xz (the reader must refuse it unread).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcjson.h"
#include "arczim.h"
#include "zim_fixture.h"

#define SPEC_MAX (64u << 20)

static int die(const char *what) {
    fprintf(stderr, "zim_fixture: %s\n", what);
    return 1;
}

static int64_t num(const arcjson_value *obj, const char *key, int64_t dflt) {
    int64_t v;
    return arcjson_int64(arcjson_get(obj, key), &v) ? v : dflt;
}

/* comp_code: a cluster's "compression" member as a code. */
static uint8_t comp_code(const arcjson_value *v) {
    const char *s = arcjson_string(v);
    int64_t n;
    if (s == NULL) {
        return arcjson_int64(v, &n) ? (uint8_t)n : ARCZIM_COMP_ZSTD;
    }
    return strcmp(s, "none") == 0 ? ARCZIM_COMP_NONE : strcmp(s, "xz") == 0 ? ARCZIM_COMP_XZ : ARCZIM_COMP_ZSTD;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: zim_fixture SPEC.json OUT.zim\n", stderr);
        return 2;
    }
    size_t len;
    const char *why;
    char *text = arcjson_read_file(argv[1], SPEC_MAX, &len, &why);
    if (text == NULL) {
        return die(why);
    }
    arcjson_arena arena = {0};
    arcjson_error jerr;
    const arcjson_value *root = arcjson_parse(&arena, text, len, SPEC_MAX, &jerr);
    if (root == NULL) {
        return die(jerr.message);
    }
    const arcjson_value *gen = arcjson_get(root, "generate");
    size_t n_gen = (size_t)num(gen, "count", 0), k_gen = (size_t)num(gen, "clusters", 1);
    size_t gen_bytes = (size_t)num(gen, "bytes", 200);
    const arcjson_value *jc = arcjson_get(root, "clusters"), *je = arcjson_get(root, "entries");
    size_t nc = jc ? jc->u.children.count : 0, ne = je ? je->u.children.count : 0;
    zimfx_cluster *clusters = calloc(nc + (n_gen ? k_gen : 0) + 1, sizeof(*clusters));
    zimfx_entry *entries = calloc(ne + n_gen + 1, sizeof(*entries));
    char *gen_text = malloc(n_gen * (gen_bytes + 64) + 1);
    if (!clusters || !entries || !gen_text) {
        return die("out of memory");
    }
    size_t c = 0;
    for (const arcjson_value *v = jc ? jc->u.children.first : NULL; v; v = v->next, c++) {
        const arcjson_value *ext = arcjson_get(v, "extended");
        clusters[c] = (zimfx_cluster){comp_code(arcjson_get(v, "compression")), ext && ext->type == ARCJSON_TRUE};
    }
    size_t e = 0;
    for (const arcjson_value *v = je ? je->u.children.first : NULL; v; v = v->next, e++) {
        const char *ns = arcjson_string(arcjson_get(v, "ns"));
        const arcjson_value *t = arcjson_get(v, "text");
        entries[e] = (zimfx_entry){
            .ns = ns ? ns[0] : 'C',
            .path = arcjson_string(arcjson_get(v, "path")),
            .title = arcjson_string(arcjson_get(v, "title")),
            .mime = arcjson_string(arcjson_get(v, "mime")),
            .data = t ? t->u.string.ptr : "",
            .size = t ? t->u.string.len : 0,
            .cluster = (unsigned)num(v, "cluster", 0),
            .redirect = arcjson_string(arcjson_get(v, "redirect")),
        };
        if (entries[e].path == NULL || (!entries[e].redirect && entries[e].mime == NULL)) {
            return die("every entry needs a path, and a mime or a redirect");
        }
    }
    for (size_t k = 0; n_gen && k < k_gen; k++) {
        clusters[c + k] = (zimfx_cluster){ARCZIM_COMP_ZSTD, false};
    }
    char *g = gen_text;
    for (size_t i = 0; i < n_gen; i++) {
        char *path = g;
        g += sprintf(g, "gen/%07zu", i) + 1;
        char *body = g;
        int w = sprintf(g, "<p>Generated article %zu.", i);
        for (size_t b = (size_t)w; b + 6 < gen_bytes; b += 6) {
            memcpy(g + b, " word.", 6);
            w = (int)(b + 6);
        }
        g += w;
        entries[e + i] = (zimfx_entry){
            'C', path, NULL, "text/html", body, (size_t)(g - body), (unsigned)(c + (i * 7919) % k_gen), NULL};
        *g++ = '\0';
    }
    zimfx_spec spec = {
        .major = (uint16_t)num(root, "major", 6),
        .minor = (uint16_t)num(root, "minor", 3),
        .entries = entries,
        .n_entries = e + n_gen,
        .clusters = clusters,
        .n_clusters = c + (n_gen ? k_gen : 0),
        .main_page = arcjson_string(arcjson_get(root, "main_page")),
    };
    const arcjson_value *tl = arcjson_get(root, "title_list");
    spec.title_list = tl && tl->type == ARCJSON_TRUE;
    uint8_t *zim;
    size_t zim_len;
    char err[256];
    if (!zimfx_build(&spec, &zim, &zim_len, NULL, err, sizeof(err))) {
        return die(err);
    }
    if (!zimfx_write_file(argv[2], zim, zim_len)) {
        return die("cannot write the output file");
    }
    free(zim);
    free(gen_text);
    free(entries);
    free(clusters);
    arcjson_arena_free(&arena);
    free(text);
    return 0;
}
