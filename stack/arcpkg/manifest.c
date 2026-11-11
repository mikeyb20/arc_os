/*
 * manifest.c — a package's manifest.json, checked against the rules a reader must enforce
 * (schemas/arcpkg.schema.json and schemas/README.md; arcpkg.h).
 *
 * Checked: schema_version 1, kind, name (id), version (semver), arch and install_root for the kind,
 * install_root = /<apps|models|packs>/<name> (the layout lane-l.md pins and arc-llm and arcsearch
 * read), requires.ram_mb and disk_bytes, requires.packages names, license and source, every file
 * (relative path, size, SHA-256) listed once, and the model or pack object's name and version equal
 * to the package's. Unknown fields are ignored (schemas are additive). The file list is sorted once
 * (O(n log n)) so lookups during install are O(log n).
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcpkg.h"

bool pkg_fail(char *why, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, PKG_WHY_MAX, fmt, ap);
    va_end(ap);
    return false;
}

const char *pkg_kind_name(pkg_kind k) {
    return k == PKG_PROGRAM ? "program" : k == PKG_MODEL ? "model" : "pack";
}

const char *pkg_area_name(pkg_kind k) {
    return k == PKG_PROGRAM ? "apps" : k == PKG_MODEL ? "models" : "packs";
}

/* str: member `key` as a non-empty string, or NULL. */
static const char *str(const arcjson_value *obj, const char *key) {
    const char *s = arcjson_string(arcjson_get(obj, key));
    return s != NULL && s[0] != '\0' ? s : NULL;
}

/* uint_member: member `key` as an integer >= 0. */
static bool uint_member(const arcjson_value *obj, const char *key, uint64_t *out) {
    int64_t v;
    if (!arcjson_int64(arcjson_get(obj, key), &v) || v < 0) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static bool is_sha256(const char *s) {
    if (s == NULL || strlen(s) != 64) {
        return false;
    }
    return strspn(s, "0123456789abcdef") == 64;
}

static int file_cmp(const void *a, const void *b) {
    return strcmp(((const pkg_file *)a)->path, ((const pkg_file *)b)->path);
}

/* load_files: the `files` array into m->files, sorted, each checked, no path twice. */
static bool load_files(pkg_manifest *m, const arcjson_value *files, char *why) {
    if (files == NULL || files->type != ARCJSON_ARRAY || files->u.children.count == 0) {
        return pkg_fail(why, "manifest.json: `files` must be a non-empty array");
    }
    m->files = calloc(files->u.children.count, sizeof(pkg_file));
    if (m->files == NULL) {
        return pkg_fail(why, "out of memory");
    }
    for (const arcjson_value *f = files->u.children.first; f != NULL; f = f->next) {
        pkg_file *e = &m->files[m->n_files];
        const arcjson_value *path = arcjson_get(f, "path");
        e->path = arcjson_string(path);
        e->sha256 = arcjson_string(arcjson_get(f, "sha256"));
        if (e->path == NULL || !arcjson_is_schema_relpath(e->path, path->u.string.len) ||
            !uint_member(f, "size", &e->size) || !is_sha256(e->sha256)) {
            return pkg_fail(why,
                            "manifest.json: file %zu needs a relative `path`, a `size` >= 0 and a lowercase `sha256`",
                            m->n_files);
        }
        m->size += e->size;
        m->n_files++;
    }
    qsort(m->files, m->n_files, sizeof(pkg_file), file_cmp);
    for (size_t i = 1; i < m->n_files; i++) {
        if (strcmp(m->files[i - 1].path, m->files[i].path) == 0) {
            return pkg_fail(why, "manifest.json: file '%s' is listed twice", m->files[i].path);
        }
    }
    return true;
}

/* check_kind: arch, install_root and the model or pack object for the package's kind. */
static bool check_kind(pkg_manifest *m, const char *kind, const char *arch, char *why) {
    if (kind == NULL || (strcmp(kind, "program") != 0 && strcmp(kind, "model") != 0 && strcmp(kind, "pack") != 0)) {
        return pkg_fail(why, "manifest.json: `kind` must be program, model or pack");
    }
    m->kind = kind[0] == 'p' ? (kind[1] == 'r' ? PKG_PROGRAM : PKG_PACK) : PKG_MODEL;
    if (arch == NULL || strcmp(arch, m->kind == PKG_PROGRAM ? "x86_64" : "any") != 0) {
        return pkg_fail(why, "manifest.json: `arch` must be %s for a %s", m->kind == PKG_PROGRAM ? "x86_64" : "any",
                        kind);
    }
    char want[80];
    snprintf(want, sizeof(want), "/%s/%s", pkg_area_name(m->kind), m->name);
    if (m->install_root == NULL || strcmp(m->install_root, want) != 0) {
        return pkg_fail(why, "manifest.json: `install_root` must be %s (lane-l.md layout)", want);
    }
    if (m->kind == PKG_PROGRAM) {
        if (str(m->doc, "cpu_level") == NULL || arcjson_get(m->doc, "model") || arcjson_get(m->doc, "pack")) {
            return pkg_fail(why, "manifest.json: a program needs `cpu_level` and has no `model` or `pack`");
        }
        return true;
    }
    const char *own = m->kind == PKG_MODEL ? "model" : "pack";
    const arcjson_value *obj = arcjson_get(m->doc, own);
    if (obj == NULL || obj->type != ARCJSON_OBJECT || arcjson_get(m->doc, m->kind == PKG_MODEL ? "pack" : "model")) {
        return pkg_fail(why, "manifest.json: a %s needs a `%s` object and no other", kind, own);
    }
    const char *n = str(obj, "name");
    const char *v = str(obj, "version");
    if (n == NULL || v == NULL || strcmp(n, m->name) != 0 || strcmp(v, m->version) != 0) {
        return pkg_fail(why, "manifest.json: `%s` name and version must equal the package's (%s %s)", own, m->name,
                        m->version);
    }
    uint64_t model_ram = 0;
    if (m->kind == PKG_MODEL && uint_member(obj, "ram_mb", &model_ram) && model_ram > m->ram_mb) {
        m->ram_mb = model_ram;
    }
    return true;
}

/* check_requires: requires.ram_mb, disk_bytes and the names in packages. */
static bool check_requires(pkg_manifest *m, char *why) {
    const arcjson_value *req = arcjson_get(m->doc, "requires");
    uint64_t disk;
    if (req == NULL || !uint_member(req, "ram_mb", &m->ram_mb) || !uint_member(req, "disk_bytes", &disk)) {
        return pkg_fail(why, "manifest.json: `requires` needs `ram_mb` and `disk_bytes` (integers >= 0)");
    }
    m->packages = arcjson_get(req, "packages");
    if (m->packages == NULL) {
        return true;
    }
    if (m->packages->type != ARCJSON_ARRAY) {
        return pkg_fail(why, "manifest.json: `requires.packages` must be an array");
    }
    for (const arcjson_value *p = m->packages->u.children.first; p != NULL; p = p->next) {
        const char *n = str(p, "name");
        const char *min = arcjson_string(arcjson_get(p, "min_version"));
        if (n == NULL || !arcjson_is_schema_id(n, strlen(n)) ||
            (min != NULL && !arcjson_is_schema_semver(min, strlen(min)))) {
            return pkg_fail(why, "manifest.json: `requires.packages` entries need an id `name` and a semver "
                                 "`min_version` if any");
        }
    }
    return true;
}

bool pkg_manifest_from(pkg_manifest *m, const arcjson_value *doc, char *why) {
    int64_t schema;
    m->doc = doc;
    if (doc == NULL || doc->type != ARCJSON_OBJECT || !arcjson_int64(arcjson_get(doc, "schema_version"), &schema) ||
        schema != 1) {
        return pkg_fail(why, "manifest.json: not a schema_version 1 package manifest");
    }
    m->name = str(doc, "name");
    m->version = str(doc, "version");
    m->install_root = str(doc, "install_root");
    if (m->name == NULL || !arcjson_is_schema_id(m->name, strlen(m->name))) {
        return pkg_fail(why, "manifest.json: `name` must be an id (a-z, 0-9, '-', at most 64)");
    }
    if (m->version == NULL || !arcjson_is_schema_semver(m->version, strlen(m->version))) {
        return pkg_fail(why, "manifest.json: `version` must be semver");
    }
    if (str(doc, "license") == NULL || str(doc, "source") == NULL) {
        return pkg_fail(why, "manifest.json: `license` and `source` are required");
    }
    return check_requires(m, why) && check_kind(m, str(doc, "kind"), str(doc, "arch"), why) &&
           load_files(m, arcjson_get(doc, "files"), why);
}

bool pkg_manifest_parse(pkg_manifest *m, const char *text, size_t len, char *why) {
    memset(m, 0, sizeof(*m));
    arcjson_arena_init(&m->arena);
    arcjson_error err;
    const arcjson_value *doc = arcjson_parse(&m->arena, text, len, PKG_MANIFEST_MAX, &err);
    if (doc == NULL) {
        return pkg_fail(why, "manifest.json: %s at byte %zu", err.message, err.offset);
    }
    return pkg_manifest_from(m, doc, why);
}

void pkg_manifest_free(pkg_manifest *m) {
    free(m->files);
    m->files = NULL;
    arcjson_arena_free(&m->arena);
}

pkg_file *pkg_manifest_find(const pkg_manifest *m, const char *path, size_t len) {
    size_t lo = 0, hi = m->n_files;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strncmp(m->files[mid].path, path, len);
        if (c == 0 && m->files[mid].path[len] != '\0') {
            c = 1; /* longer than path[0..len): sorts after it */
        }
        if (c == 0) {
            return &m->files[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}
