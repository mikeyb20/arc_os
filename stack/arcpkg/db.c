/*
 * db.c — the package database and the model roles (arcpkg.h).
 *
 * <root>/var/lib/arcpkg/<name>.json, written only with arcutil_write_durable:
 *   {"schema_version": 1, "name": ..., "versions": [{"version": ..., "manifest": {package manifest}}]}
 * The manifest kept for each version is the one it was installed from, so verify, list and remove
 * need nothing else. <root>/models/roles.json follows schemas/model-roles.schema.json.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arcpkg.h"

#define ROLES_MAX (256u * 1024u) /* as arc-llm reads it */

bool pkg_db_path(const char *root, const char *name, char *buf, size_t cap) {
    int n = name ? snprintf(buf, cap, "%s/var/lib/arcpkg/%s.json", root, name)
                 : snprintf(buf, cap, "%s/var/lib/arcpkg", root);
    return n >= 0 && (size_t)n < cap;
}

static int version_order(const void *a, const void *b) {
    return arcjson_schema_version_cmp(*(const char *const *)a, *(const char *const *)b);
}

bool pkg_db_load(const char *root, const char *name, pkg_db *db, char *why) {
    char path[4096];
    memset(db, 0, sizeof(*db));
    arcjson_arena_init(&db->arena);
    snprintf(db->name, sizeof(db->name), "%s", name);
    if (!pkg_db_path(root, name, path, sizeof(path))) {
        return pkg_fail(why, "root path too long");
    }
    size_t len;
    const char *err;
    db->text = arcjson_read_file(path, PKG_DB_MAX, &len, &err);
    if (db->text == NULL) {
        return access(path, F_OK) != 0 || pkg_fail(why, "%s: %s", path, err);
    }
    arcjson_error jerr;
    const arcjson_value *doc = arcjson_parse(&db->arena, db->text, len, PKG_DB_MAX, &jerr);
    const arcjson_value *versions = arcjson_get(doc, "versions");
    if (doc == NULL || versions == NULL || versions->type != ARCJSON_ARRAY ||
        versions->u.children.count > PKG_DB_VERSIONS_MAX) {
        return pkg_fail(why, "%s: damaged package database entry", path);
    }
    const char *order[PKG_DB_VERSIONS_MAX];
    for (const arcjson_value *v = versions->u.children.first; v != NULL; v = v->next) {
        const char *ver = arcjson_string(arcjson_get(v, "version"));
        const arcjson_value *man = arcjson_get(v, "manifest");
        if (ver == NULL || !arcjson_is_schema_semver(ver, strlen(ver)) || man == NULL) {
            return pkg_fail(why, "%s: damaged package database entry", path);
        }
        order[db->n_versions++] = ver;
    }
    qsort(order, db->n_versions, sizeof(order[0]), version_order);
    for (size_t i = 0; i < db->n_versions; i++) { /* manifests in the sorted order */
        db->versions[i] = order[i];
        for (const arcjson_value *v = versions->u.children.first; v != NULL; v = v->next) {
            if (arcjson_string(arcjson_get(v, "version")) == order[i]) {
                db->manifests[i] = arcjson_get(v, "manifest");
            }
        }
    }
    return true;
}

void pkg_db_free(pkg_db *db) {
    arcjson_arena_free(&db->arena);
    free(db->text);
    db->text = NULL;
}

int pkg_db_find(const pkg_db *db, const char *version) {
    for (size_t i = 0; i < db->n_versions; i++) {
        if (strcmp(db->versions[i], version) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* write_version: one {"version", "manifest"} entry. */
static void write_version(arcjson_writer *w, const char *version, const arcjson_value *manifest) {
    arcjson_write_object_begin(w);
    arcjson_write_key(w, "version");
    arcjson_write_string(w, version);
    arcjson_write_key(w, "manifest");
    arcjson_write_value(w, manifest);
    arcjson_write_object_end(w);
}

bool pkg_db_save(const char *root, const pkg_db *db, const arcjson_value *add, const char *drop, char *why) {
    char dir[4096];
    char path[4096];
    if (!pkg_db_path(root, NULL, dir, sizeof(dir)) || !pkg_db_path(root, db->name, path, sizeof(path))) {
        return pkg_fail(why, "root path too long");
    }
    size_t kept = 0;
    arcjson_writer w;
    arcjson_writer_init(&w);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "schema_version");
    arcjson_write_int(&w, 1);
    arcjson_write_key(&w, "name");
    arcjson_write_string(&w, db->name);
    arcjson_write_key(&w, "versions");
    arcjson_write_array_begin(&w);
    const char *add_version = add ? arcjson_string(arcjson_get(add, "version")) : NULL;
    for (size_t i = 0; i <= db->n_versions; i++) { /* lowest version first, `add` in its place */
        if (add_version != NULL &&
            (i == db->n_versions || arcjson_schema_version_cmp(add_version, db->versions[i]) < 0)) {
            write_version(&w, add_version, add);
            add_version = NULL;
            kept++;
        }
        if (i < db->n_versions && (drop == NULL || strcmp(db->versions[i], drop) != 0)) {
            write_version(&w, db->versions[i], db->manifests[i]);
            kept++;
        }
    }
    arcjson_write_array_end(&w);
    arcjson_write_object_end(&w);
    size_t len;
    const char *text = arcjson_writer_result(&w, &len);
    bool ok;
    if (text == NULL) {
        ok = pkg_fail(why, "out of memory");
    } else if (kept > PKG_DB_VERSIONS_MAX) {
        ok = pkg_fail(why, "%s: more than %u versions installed", db->name, PKG_DB_VERSIONS_MAX);
    } else if (kept == 0) {
        ok = (unlink(path) == 0 || errno == ENOENT) && arcutil_sync_dir(dir);
        ok = ok || pkg_fail(why, "%s: %s", path, strerror(errno));
    } else {
        ok = arcutil_make_dirs(dir) && arcutil_write_durable(path, text, len);
        ok = ok || pkg_fail(why, "%s: %s", path, strerror(errno));
    }
    arcjson_writer_free(&w);
    return ok;
}

/* ---- roles.json --------------------------------------------------------------------------------- */

/* load_roles: parse <root>/models/roles.json into arena; NULL (no error) if it doesn't exist. */
static const arcjson_value *load_roles(const char *root, arcjson_arena *arena, char **text, char *path, size_t cap,
                                       char *why, bool *ok) {
    *ok = snprintf(path, cap, "%s/models/roles.json", root) < (int)cap || pkg_fail(why, "root path too long");
    size_t len;
    const char *err;
    *text = *ok ? arcjson_read_file(path, ROLES_MAX, &len, &err) : NULL;
    if (*text == NULL) {
        *ok = *ok && (access(path, F_OK) != 0 || pkg_fail(why, "%s: %s", path, err));
        return NULL;
    }
    arcjson_error jerr;
    const arcjson_value *doc = arcjson_parse(arena, *text, len, ROLES_MAX, &jerr);
    const arcjson_value *roles = arcjson_get(doc, "roles");
    if (roles == NULL || roles->type != ARCJSON_OBJECT) {
        *ok = pkg_fail(why, "%s: needs a `roles` object (schemas/model-roles.schema.json)", path);
        return NULL;
    }
    return roles;
}

bool pkg_role_uses(const char *root, const char *name, const char *version, char *role, size_t cap) {
    arcjson_arena arena;
    arcjson_arena_init(&arena);
    char *text;
    char path[4096];
    char why[PKG_WHY_MAX];
    bool ok;
    const arcjson_value *roles = load_roles(root, &arena, &text, path, sizeof(path), why, &ok);
    bool used = false;
    for (const arcjson_value *r = roles ? roles->u.children.first : NULL; r != NULL && !used; r = r->next) {
        const char *n = arcjson_string(arcjson_get(r, "name"));
        const char *v = arcjson_string(arcjson_get(r, "version"));
        if (n != NULL && v != NULL && strcmp(n, name) == 0 && strcmp(v, version) == 0) {
            snprintf(role, cap, "%s", r->key);
            used = true;
        }
    }
    arcjson_arena_free(&arena);
    free(text);
    return used;
}

bool pkg_roles_show(const char *root, char *why) {
    arcjson_arena arena;
    arcjson_arena_init(&arena);
    char *text;
    char path[4096];
    bool ok;
    const arcjson_value *roles = load_roles(root, &arena, &text, path, sizeof(path), why, &ok);
    for (const arcjson_value *r = roles ? roles->u.children.first : NULL; r != NULL; r = r->next) {
        const char *n = arcjson_string(arcjson_get(r, "name"));
        const char *v = arcjson_string(arcjson_get(r, "version"));
        printf("%s\t%s\t%s\n", r->key, n ? n : "?", v ? v : "?");
    }
    arcjson_arena_free(&arena);
    free(text);
    return ok;
}

/* model_lists_role: <root>/models/<name>/<version>/model.json lists `role`. */
static bool model_lists_role(const char *root, const char *name, const char *version, const char *role, char *why) {
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/models/%s/%s/model.json", root, name, version) >= (int)sizeof(path)) {
        return pkg_fail(why, "root path too long");
    }
    size_t len;
    const char *err;
    char *text = arcjson_read_file(path, ROLES_MAX, &len, &err);
    if (text == NULL) {
        return pkg_fail(why, "%s: %s", path, err);
    }
    arcjson_arena arena;
    arcjson_arena_init(&arena);
    const arcjson_value *roles = arcjson_get(arcjson_parse(&arena, text, len, ROLES_MAX, NULL), "roles");
    bool listed = false;
    for (const arcjson_value *r = roles && roles->type == ARCJSON_ARRAY ? roles->u.children.first : NULL; r != NULL;
         r = r->next) {
        listed = listed || (arcjson_string(r) != NULL && strcmp(arcjson_string(r), role) == 0);
    }
    arcjson_arena_free(&arena);
    free(text);
    return listed || pkg_fail(why, "%s does not list the role %s", path, role);
}

bool pkg_roles_set(const char *root, const char *role, const char *name, const char *version, char *why) {
    if (!arcjson_is_schema_role(role, strlen(role))) {
        return pkg_fail(why, "'%s' is not a role (chat, embed, router, expert:<domain>)", role);
    }
    pkg_db db;
    bool ok = pkg_db_load(root, name, &db, why);
    const char *kind = ok && db.n_versions ? arcjson_string(arcjson_get(db.manifests[0], "kind")) : NULL;
    if (ok && (pkg_db_find(&db, version) < 0 || kind == NULL || strcmp(kind, "model") != 0)) {
        ok = pkg_fail(why, "model %s %s is not installed", name, version);
    }
    pkg_db_free(&db);
    if (!ok || !model_lists_role(root, name, version, role, why)) {
        return false;
    }
    arcjson_arena arena;
    arcjson_arena_init(&arena);
    char *text;
    char path[4096];
    const arcjson_value *roles = load_roles(root, &arena, &text, path, sizeof(path), why, &ok);
    arcjson_writer w;
    arcjson_writer_init(&w);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "schema_version");
    arcjson_write_int(&w, 1);
    arcjson_write_key(&w, "roles");
    arcjson_write_object_begin(&w);
    for (const arcjson_value *r = roles ? roles->u.children.first : NULL; r != NULL; r = r->next) {
        if (strcmp(r->key, role) != 0) { /* every other role stays as it was */
            arcjson_write_key(&w, r->key);
            arcjson_write_value(&w, r);
        }
    }
    arcjson_write_key(&w, role);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "name");
    arcjson_write_string(&w, name);
    arcjson_write_key(&w, "version");
    arcjson_write_string(&w, version);
    arcjson_write_object_end(&w);
    arcjson_write_object_end(&w);
    arcjson_write_object_end(&w);
    size_t len;
    const char *out = arcjson_writer_result(&w, &len);
    if (ok && (out == NULL || !arcutil_write_durable(path, out, len))) {
        ok = pkg_fail(why, "%s: %s", path, out ? strerror(errno) : "out of memory");
    }
    arcjson_writer_free(&w);
    arcjson_arena_free(&arena);
    free(text);
    return ok;
}
