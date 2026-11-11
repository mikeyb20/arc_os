/*
 * roles.c — the model registry (roles.h). Files are small (roles.json, model.json up to a few
 * KiB with a chat template), read whole and parsed with arcjson; nothing here is per token.
 */
#define _GNU_SOURCE
#include "roles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "arcjson.h"
#include "engine.h"

#define MANIFEST_MAX (256u * 1024u) /* roles.json and model.json size limit */

/* read_manifest: the whole file at `path` (at most MANIFEST_MAX bytes), malloc'd; NULL with *why set. */
static char *read_manifest(const char *path, size_t *len, char **why) {
    const char *err;
    char *text = arcjson_read_file(path, MANIFEST_MAX, len, &err);
    if (text == NULL) {
        asprintf(why, "%s: %s", path, err);
    }
    return text;
}

/* is_id, is_version: the schemas' rules, from arcjson (one home, ADR-0020). */
static bool is_id(const char *s) {
    return arcjson_is_schema_id(s, strlen(s));
}

static bool is_version(const char *s) {
    return arcjson_is_schema_version(s, strlen(s));
}

/* is_role: chat, embed, router or expert:<id>. */
static bool is_role(const char *s) {
    if (strcmp(s, "chat") == 0 || strcmp(s, "embed") == 0 || strcmp(s, "router") == 0) {
        return true;
    }
    return strncmp(s, "expert:", 7) == 0 && is_id(s + 7);
}

/* is_model_file: a relative path inside the model directory, no `.` or `..` parts, ending .gguf. */
static bool is_model_file(const char *s) {
    size_t n = strlen(s);
    if (n < 6 || strcmp(s + n - 5, ".gguf") != 0 || s[0] == '/') {
        return false;
    }
    for (const char *part = s; part != NULL;) {
        const char *slash = strchr(part, '/');
        size_t len = slash ? (size_t)(slash - part) : strlen(part);
        if (len == 0 || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) {
            return false;
        }
        part = slash ? slash + 1 : NULL;
    }
    return true;
}

/* positive_int: member `key` of `obj` as an integer >= 1, or 0. */
static int64_t positive_int(const arcjson_value *obj, const char *key) {
    int64_t v = 0;
    return arcjson_int64(arcjson_get(obj, key), &v) && v >= 1 ? v : 0;
}

/* listed_roles: model.json's `roles` as "\nrole\nrole\n" for role_listed(), malloc'd; NULL if invalid. */
static char *listed_roles(const arcjson_value *roles) {
    if (roles == NULL || roles->type != ARCJSON_ARRAY || roles->u.children.count == 0) {
        return NULL;
    }
    size_t len = 2;
    for (const arcjson_value *v = roles->u.children.first; v != NULL; v = v->next) {
        const char *s = arcjson_string(v);
        if (s == NULL || !is_role(s)) {
            return NULL;
        }
        len += strlen(s) + 1;
    }
    char *out = malloc(len);
    if (out == NULL) {
        return NULL;
    }
    char *p = out;
    *p++ = '\n';
    for (const arcjson_value *v = roles->u.children.first; v != NULL; v = v->next) {
        size_t n = strlen(v->u.string.ptr);
        memcpy(p, v->u.string.ptr, n);
        p += n;
        *p++ = '\n';
    }
    *p = '\0';
    return out;
}

/* role_listed: model.json of m lists `role`. */
static bool role_listed(const llm_model *m, const char *role) {
    if (m->listed_roles == NULL) {
        return false;
    }
    for (const char *p = strstr(m->listed_roles, role); p != NULL; p = strstr(p + 1, role)) {
        if (p[-1] == '\n' && p[strlen(role)] == '\n') {
            return true;
        }
    }
    return false;
}

/* load_manifest: fill m from <root>/models/<name>/<version>/model.json; on a problem set m->error. */
static void load_manifest(const llm_registry *r, llm_model *m) {
    char *dir = NULL, *path = NULL, *why = NULL;
    asprintf(&dir, "%s/models/%s/%s", r->root, m->name, m->version);
    asprintf(&path, "%s/model.json", dir);
    size_t len = 0;
    char *text = read_manifest(path, &len, &why);
    arcjson_arena arena;
    arcjson_arena_init(&arena);
    arcjson_error jerr = {0, NULL};
    const arcjson_value *doc = text ? arcjson_parse(&arena, text, len, MANIFEST_MAX, &jerr) : NULL;
    const char *file = arcjson_string(arcjson_get(doc, "file"));
    const char *tmpl = arcjson_string(arcjson_get(doc, "chat_template"));
    if (text != NULL && doc == NULL) {
        asprintf(&why, "%s: invalid JSON at byte %zu: %s", path, jerr.offset, jerr.message);
    } else if (doc != NULL && (arcjson_string(arcjson_get(doc, "name")) == NULL ||
                               strcmp(arcjson_string(arcjson_get(doc, "name")), m->name) != 0 ||
                               arcjson_string(arcjson_get(doc, "version")) == NULL ||
                               strcmp(arcjson_string(arcjson_get(doc, "version")), m->version) != 0)) {
        asprintf(&why, "%s: name and version don't match its directory", path);
    } else if (doc != NULL && (file == NULL || !is_model_file(file))) {
        asprintf(&why, "%s: `file` is not a .gguf path inside the model directory", path);
    } else if (doc != NULL && (positive_int(doc, "ram_mb") == 0 || positive_int(doc, "context_length") == 0)) {
        asprintf(&why, "%s: `ram_mb` and `context_length` must be integers >= 1", path);
    } else if (doc != NULL && (m->listed_roles = listed_roles(arcjson_get(doc, "roles"))) == NULL) {
        asprintf(&why, "%s: `roles` must be a non-empty array of roles", path);
    } else if (doc != NULL) {
        asprintf(&m->gguf_path, "%s/%s", dir, file);
        m->chat_template = tmpl ? strdup(tmpl) : NULL;
        m->ram_mb = positive_int(doc, "ram_mb");
        m->context_length = positive_int(doc, "context_length");
        m->embedder = arcjson_get(doc, "embedder") != NULL;
    }
    m->error = why;
    arcjson_arena_free(&arena);
    free(text);
    free(path);
    free(dir);
}

/* model_free: unload and free one model. */
static void model_free(llm_model *m) {
    engine_unload(m);
    free(m->name);
    free(m->version);
    free(m->gguf_path);
    free(m->chat_template);
    free(m->listed_roles);
    free(m->error);
    free(m);
}

/* clear: free roles and models (unloading them) and the error. */
static void clear(llm_registry *r) {
    for (size_t i = 0; i < r->n_roles; i++) {
        free(r->roles[i].role);
        free(r->roles[i].error);
    }
    free(r->roles);
    r->roles = NULL;
    r->n_roles = 0;
    while (r->models != NULL) {
        llm_model *next = r->models->next;
        model_free(r->models);
        r->models = next;
    }
    free(r->error);
    r->error = NULL;
}

/* find_model: the model `name`@`version` in list `m`, or NULL. */
static llm_model *find_model(llm_model *m, const char *name, const char *version) {
    for (; m != NULL; m = m->next) {
        if (strcmp(m->name, name) == 0 && strcmp(m->version, version) == 0) {
            return m;
        }
    }
    return NULL;
}

/*
 * parse_roles: build the role table and model list of `r` from roles.json in `doc`.
 * Loaded models from `old` that are still named move over (weights stay loaded).
 */
static void parse_roles(llm_registry *r, const arcjson_value *doc, llm_model **old) {
    const arcjson_value *roles = arcjson_get(doc, "roles");
    int64_t schema = 0;
    if (!arcjson_int64(arcjson_get(doc, "schema_version"), &schema) || schema != 1 || roles == NULL ||
        roles->type != ARCJSON_OBJECT) {
        asprintf(&r->error, "%s: needs schema_version 1 and a `roles` object", r->roles_path);
        return;
    }
    r->roles = calloc(roles->u.children.count ? roles->u.children.count : 1, sizeof(*r->roles));
    llm_model **tail = &r->models;
    for (const arcjson_value *v = roles->u.children.first; v != NULL && r->roles != NULL; v = v->next) {
        const char *name = arcjson_string(arcjson_get(v, "name"));
        const char *version = arcjson_string(arcjson_get(v, "version"));
        if (!is_role(v->key) || name == NULL || !is_id(name) || version == NULL || !is_version(version)) {
            fprintf(stderr, "arc-llm: %s: role %s skipped: needs a valid role, `name` and `version`\n", r->roles_path,
                    v->key);
            continue;
        }
        llm_model *m = find_model(r->models, name, version);
        if (m == NULL) {
            llm_model **prev = old;
            while (*prev != NULL && !(strcmp((*prev)->name, name) == 0 && strcmp((*prev)->version, version) == 0)) {
                prev = &(*prev)->next;
            }
            if (*prev != NULL) { /* still named: keep it if loaded, else re-read its model.json */
                m = *prev;
                *prev = m->next;
                if (m->model == NULL) {
                    model_free(m);
                    m = NULL;
                }
            }
            if (m == NULL) {
                m = calloc(1, sizeof(*m));
                if (m == NULL) {
                    continue;
                }
                m->name = strdup(name);
                m->version = strdup(version);
                load_manifest(r, m);
            }
            m->next = NULL;
            *tail = m;
            tail = &m->next;
        }
        llm_role *role = &r->roles[r->n_roles++];
        role->role = strdup(v->key);
        role->model = m;
        if (m->error == NULL && !role_listed(m, v->key)) {
            asprintf(&role->error, "roles.json gives %s@%s role %s, which its model.json doesn't list", name, version,
                     v->key);
        }
    }
}

void roles_init(llm_registry *r, const char *root) {
    memset(r, 0, sizeof(*r));
    r->root = strdup(root);
    asprintf(&r->roles_path, "%s/models/roles.json", root);
}

void roles_free(llm_registry *r) {
    clear(r);
    free(r->root);
    free(r->roles_path);
    memset(r, 0, sizeof(*r));
}

void roles_refresh(llm_registry *r) {
    struct stat st;
    bool exists = stat(r->roles_path, &st) == 0;
    if (r->checked && exists == r->seen &&
        (!exists || (st.st_dev == r->dev && st.st_ino == r->ino && st.st_size == r->size &&
                     st.st_mtim.tv_sec == r->mtime.tv_sec && st.st_mtim.tv_nsec == r->mtime.tv_nsec))) {
        return;
    }
    r->checked = true;
    r->seen = exists;
    if (exists) {
        r->dev = st.st_dev;
        r->ino = st.st_ino;
        r->size = st.st_size;
        r->mtime = st.st_mtim;
    }

    /* Detach the current models; parse_roles takes back the ones still named. */
    llm_model *old = r->models;
    r->models = NULL;
    clear(r);

    size_t len = 0;
    char *text = read_manifest(r->roles_path, &len, &r->error);
    if (text != NULL) {
        arcjson_arena arena;
        arcjson_arena_init(&arena);
        arcjson_error jerr = {0, NULL};
        const arcjson_value *doc = arcjson_parse(&arena, text, len, MANIFEST_MAX, &jerr);
        if (doc == NULL) {
            asprintf(&r->error, "%s: invalid JSON at byte %zu: %s", r->roles_path, jerr.offset, jerr.message);
        } else {
            parse_roles(r, doc, &old);
        }
        arcjson_arena_free(&arena);
        free(text);
    }
    while (old != NULL) { /* no role names these any more */
        llm_model *next = old->next;
        fprintf(stderr, "arc-llm: %s@%s no longer has a role%s\n", old->name, old->version,
                old->model ? "; unloaded" : "");
        model_free(old);
        old = next;
    }
    fprintf(stderr, "arc-llm: %s: %zu role(s)%s%s\n", r->roles_path, r->n_roles, r->error ? "; " : "",
            r->error ? r->error : "");
}

llm_model *roles_lookup(const llm_registry *r, const char *want, const char **why) {
    *why = NULL;
    for (size_t i = 0; i < r->n_roles; i++) {
        if (strcmp(r->roles[i].role, want) == 0) {
            llm_model *m = r->roles[i].model;
            *why = r->roles[i].error ? r->roles[i].error : m->error;
            return m;
        }
    }
    for (llm_model *m = r->models; m != NULL; m = m->next) {
        if (strcmp(m->name, want) == 0) {
            *why = m->error;
            return m;
        }
    }
    return NULL;
}
