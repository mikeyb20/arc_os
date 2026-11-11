/*
 * arcpkg — install, remove, list and verify .arcpkg packages and switch model roles (M5-02; ADR-0010,
 * ADR-0012; arcpkg.h). Offline: a package is a file path.
 *
 *   arcpkg [--root R] [--force] install FILE
 *   arcpkg [--root R] [--force] remove NAME [VERSION]
 *   arcpkg [--root R] list
 *   arcpkg [--root R] verify [NAME]
 *   arcpkg [--root R] role [ROLE NAME VERSION]
 *
 * Exit 0 on success, 1 on failure (the reason on stderr), 2 on a usage error. One arcpkg runs at a
 * time: at start it removes the .tmp-* directories an interrupted install left under
 * <root>/{apps,models,packs}/<name>/.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arcpkg.h"

#define HASH_BUF (64u * 1024u)

typedef struct {
    char root[2048]; /* without a trailing '/'; "" for the real root */
    bool force;
    const char *args[4];
    int n_args;
} options;

static int arcpkg_usage(void) {
    fprintf(stderr, "usage: arcpkg [--root R] [--force] install FILE | remove NAME [VERSION] | list |\n"
                    "              verify [NAME] | role [ROLE NAME VERSION]\n");
    return 2;
}

static int cli_fail(const char *why) {
    fprintf(stderr, "arcpkg: %s\n", why);
    return 1;
}

/* cleanup_area: remove <root>/<area>/<name>/.tmp-* left by an interrupted install. */
static void cleanup_area(const char *root, const char *area) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/%s", root, area);
    DIR *d = opendir(dir);
    for (struct dirent *e; d != NULL && (e = readdir(d)) != NULL;) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char sub[4096];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name) >= (int)sizeof(sub)) {
            continue;
        }
        DIR *s = opendir(sub);
        for (struct dirent *t; s != NULL && (t = readdir(s)) != NULL;) {
            char tmp[4096];
            if (strncmp(t->d_name, ".tmp-", 5) == 0 &&
                snprintf(tmp, sizeof(tmp), "%s/%s", sub, t->d_name) < (int)sizeof(tmp)) {
                fprintf(stderr, "arcpkg: removing %s, left by an interrupted install\n", tmp);
                arcutil_remove_tree(tmp);
            }
        }
        if (s != NULL) {
            closedir(s);
        }
    }
    if (d != NULL) {
        closedir(d);
    }
}

/* hash_file: size and SHA-256 (hex) of the file at path; false with errno if it can't be read. */
static bool hash_file(const char *path, unsigned char *buf, uint64_t *size, char hex[ARCUTIL_SHA256_HEX]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    arcutil_sha256 sha;
    arcutil_sha256_init(&sha);
    *size = 0;
    ssize_t n;
    while ((n = read(fd, buf, HASH_BUF)) != 0) {
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            int e = errno;
            close(fd);
            errno = e;
            return false;
        }
        arcutil_sha256_update(&sha, buf, (size_t)n);
        *size += (uint64_t)n;
    }
    close(fd);
    unsigned char digest[ARCUTIL_SHA256_SIZE];
    arcutil_sha256_final(&sha, digest);
    arcutil_sha256_hex(digest, hex);
    return true;
}

/* db_manifest: check the manifest recorded for db version i (as an install would) into m. */
static bool db_manifest(const pkg_db *db, size_t i, pkg_manifest *m, char *why) {
    memset(m, 0, sizeof(*m));
    arcjson_arena_init(&m->arena);
    if (!pkg_manifest_from(m, db->manifests[i], why)) {
        return false;
    }
    if (strcmp(m->name, db->name) != 0 || strcmp(m->version, db->versions[i]) != 0) {
        return pkg_fail(why, "package database entry %s %s is damaged", db->name, db->versions[i]);
    }
    return true;
}

/* verify_version: re-hash every file of db version i; print each mismatch; the number found (-1: error). */
static long verify_version(const char *root, const pkg_db *db, size_t i, unsigned char *buf, char *why) {
    pkg_manifest m;
    char dir[4096];
    if (!db_manifest(db, i, &m, why) || !pkg_version_dir(root, m.install_root, m.version, dir, sizeof(dir))) {
        pkg_manifest_free(&m);
        return pkg_fail(why, "%s %s: damaged database entry or path too long", db->name, db->versions[i]) - 1;
    }
    long bad = 0;
    for (size_t f = 0; f < m.n_files; f++) {
        char path[4096];
        uint64_t size;
        char hex[ARCUTIL_SHA256_HEX];
        const pkg_file *e = &m.files[f];
        const char *what = NULL;
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->path) >= (int)sizeof(path)) {
            what = "path too long";
        } else if (!hash_file(path, buf, &size, hex)) {
            what = errno == ENOENT ? "missing" : strerror(errno);
        } else if (size != e->size) {
            what = "size differs";
        } else if (strcmp(hex, e->sha256) != 0) {
            what = "SHA-256 differs";
        }
        if (what != NULL) {
            printf("%s %s: %s: %s\n", m.name, m.version, e->path, what);
            bad++;
        }
    }
    pkg_manifest_free(&m);
    return bad;
}

static int name_order(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* installed_names: the package names in the database, sorted (free each and the array). */
static char **installed_names(const char *root, size_t *count, char *why) {
    char dir[4096];
    *count = 0;
    if (!pkg_db_path(root, NULL, dir, sizeof(dir))) {
        pkg_fail(why, "root path too long");
        return NULL;
    }
    char **names = NULL;
    size_t cap = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d != NULL && (e = readdir(d)) != NULL;) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || strcmp(e->d_name + len - 5, ".json") != 0 || !arcjson_is_schema_id(e->d_name, len - 5)) {
            continue; /* ".", "..", "<name>.json.tmp" of a cut-short write */
        }
        if (*count == cap) {
            cap = cap ? cap * 2 : 16;
            char **grown = realloc(names, cap * sizeof(*names));
            if (grown == NULL) {
                break;
            }
            names = grown;
        }
        names[*count] = strndup(e->d_name, len - 5);
        *count += names[*count] != NULL;
    }
    if (d != NULL) {
        closedir(d);
    }
    if (names != NULL) {
        qsort(names, *count, sizeof(*names), name_order);
    }
    return names;
}

static void free_names(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

static int cmd_list(const options *o) {
    char why[PKG_WHY_MAX] = "";
    size_t count;
    char **names = installed_names(o->root, &count, why);
    bool ok = names != NULL || why[0] == '\0';
    for (size_t i = 0; ok && i < count; i++) {
        pkg_db db;
        if (!pkg_db_load(o->root, names[i], &db, why)) {
            pkg_db_free(&db);
            ok = false;
            break;
        }
        uint64_t size = 0;
        printf("%s\t%s\t", db.name, db.n_versions ? arcjson_string(arcjson_get(db.manifests[0], "kind")) : "?");
        for (size_t v = 0; ok && v < db.n_versions; v++) {
            pkg_manifest m;
            ok = db_manifest(&db, v, &m, why);
            size += m.size;
            printf("%s%s", v ? "," : "", db.versions[v]);
            pkg_manifest_free(&m);
        }
        printf("\t%llu\n", (unsigned long long)size);
        pkg_db_free(&db);
    }
    free_names(names, count);
    return ok ? 0 : cli_fail(why);
}

static int cmd_verify(const options *o) {
    char why[PKG_WHY_MAX] = "";
    size_t count = 1;
    char *one[1] = {(char *)o->args[1]};
    char **names = o->n_args == 2 ? one : installed_names(o->root, &count, why);
    unsigned char *buf = malloc(HASH_BUF);
    bool ok = buf != NULL && (names != NULL || why[0] == '\0');
    long bad = 0;
    for (size_t i = 0; ok && i < count; i++) {
        pkg_db db;
        ok = pkg_db_load(o->root, names[i], &db, why);
        if (ok && db.n_versions == 0) {
            ok = pkg_fail(why, "%s is not installed", names[i]);
        }
        for (size_t v = 0; ok && v < db.n_versions; v++) {
            long n = verify_version(o->root, &db, v, buf, why);
            ok = n >= 0;
            bad += n > 0 ? n : 0;
        }
        pkg_db_free(&db);
    }
    if (names != one) {
        free_names(names, count);
    }
    bool oom = buf == NULL;
    free(buf);
    if (!ok) {
        return cli_fail(oom && why[0] == '\0' ? "out of memory" : why);
    }
    if (bad > 0) {
        fprintf(stderr, "arcpkg: %ld file(s) do not match the package database\n", bad);
        return 1;
    }
    printf("verified\n");
    return 0;
}

/* remove_check: a model version some role points at is kept unless forced. */
static bool remove_check(const options *o, const pkg_db *db, size_t i, char *why) {
    char role[128];
    const char *kind = arcjson_string(arcjson_get(db->manifests[i], "kind"));
    if (kind == NULL || strcmp(kind, "model") != 0 ||
        !pkg_role_uses(o->root, db->name, db->versions[i], role, sizeof(role))) {
        return true;
    }
    if (o->force) {
        fprintf(stderr, "arcpkg: warning: role %s points at %s %s, removed anyway (--force)\n", role, db->name,
                db->versions[i]);
        return true;
    }
    return pkg_fail(why, "role %s points at %s %s; point it elsewhere first (arcpkg role) or use --force", role,
                    db->name, db->versions[i]);
}

/* remove_one: delete the version directory of db version i, then its database record. */
static bool remove_one(const options *o, const pkg_db *db, size_t i, char *why) {
    pkg_manifest m;
    char dir[4096];
    bool ok = db_manifest(db, i, &m, why) && pkg_version_dir(o->root, m.install_root, m.version, dir, sizeof(dir));
    if (ok && !arcutil_remove_tree(dir)) {
        ok = pkg_fail(why, "removing %s: %s", dir, strerror(errno));
    }
    pkg_manifest_free(&m);
    if (ok) {
        printf("removed %s %s\n", db->name, db->versions[i]);
    }
    return ok && pkg_db_save(o->root, db, NULL, db->versions[i], why);
}

static int cmd_remove(const options *o) {
    char why[PKG_WHY_MAX];
    const char *name = o->args[1];
    const char *version = o->n_args == 3 ? o->args[2] : NULL;
    pkg_db db;
    bool ok = pkg_db_load(o->root, name, &db, why);
    if (ok && (db.n_versions == 0 || (version != NULL && pkg_db_find(&db, version) < 0))) {
        ok = pkg_fail(why, "%s%s%s is not installed", name, version ? " " : "", version ? version : "");
    }
    for (size_t i = 0; ok && i < db.n_versions; i++) { /* refuse before removing anything */
        ok = (version != NULL && strcmp(db.versions[i], version) != 0) || remove_check(o, &db, i, why);
    }
    pkg_db_free(&db);
    while (ok) { /* one version per pass: each pass saves the database without it */
        ok = pkg_db_load(o->root, name, &db, why);
        int i = version != NULL ? pkg_db_find(&db, version) : (db.n_versions > 0 ? 0 : -1);
        bool more = ok && i >= 0;
        ok = !more || remove_one(o, &db, (size_t)i, why);
        pkg_db_free(&db);
        if (!more) {
            break;
        }
    }
    return ok ? 0 : cli_fail(why);
}

static int cmd_role(const options *o) {
    char why[PKG_WHY_MAX];
    if (o->n_args == 1) {
        return pkg_roles_show(o->root, why) ? 0 : cli_fail(why);
    }
    if (!pkg_roles_set(o->root, o->args[1], o->args[2], o->args[3], why)) {
        return cli_fail(why);
    }
    printf("role %s -> %s %s\n", o->args[1], o->args[2], o->args[3]);
    return 0;
}

/* parse: options anywhere, then a command and its arguments; false on a usage error. */
static bool parse(int argc, char **argv, options *o) {
    memset(o, 0, sizeof(*o));
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            if (snprintf(o->root, sizeof(o->root), "%s", argv[++i]) >= (int)sizeof(o->root)) {
                return false;
            }
        } else if (strcmp(argv[i], "--force") == 0) {
            o->force = true;
        } else if (argv[i][0] == '-' || o->n_args == 4) {
            return false;
        } else {
            o->args[o->n_args++] = argv[i];
        }
    }
    for (size_t len = strlen(o->root); len > 0 && o->root[len - 1] == '/'; len--) {
        o->root[len - 1] = '\0';
    }
    return o->n_args > 0;
}

/* names_ok: the NAME and VERSION arguments of remove, verify and role are ids and semver. */
static bool names_ok(const options *o, const char *cmd) {
    const char *name =
        strcmp(cmd, "role") == 0 ? (o->n_args == 4 ? o->args[2] : NULL) : (o->n_args >= 2 ? o->args[1] : NULL);
    const char *version =
        strcmp(cmd, "role") == 0 ? (o->n_args == 4 ? o->args[3] : NULL) : (o->n_args == 3 ? o->args[2] : NULL);
    return (name == NULL || arcjson_is_schema_id(name, strlen(name))) &&
           (version == NULL || arcjson_is_schema_semver(version, strlen(version)));
}

int main(int argc, char **argv) {
    options o;
    if (!parse(argc, argv, &o)) {
        return arcpkg_usage();
    }
    const char *cmd = o.args[0];
    bool shape = (strcmp(cmd, "install") == 0 && o.n_args == 2) ||
                 (strcmp(cmd, "remove") == 0 && (o.n_args == 2 || o.n_args == 3)) ||
                 (strcmp(cmd, "list") == 0 && o.n_args == 1) || (strcmp(cmd, "verify") == 0 && o.n_args <= 2) ||
                 (strcmp(cmd, "role") == 0 && (o.n_args == 1 || o.n_args == 4));
    if (!shape || (strcmp(cmd, "install") != 0 && !names_ok(&o, cmd))) {
        return arcpkg_usage();
    }
    for (pkg_kind k = PKG_PROGRAM; k <= PKG_PACK; k++) {
        cleanup_area(o.root, pkg_area_name(k));
    }
    if (strcmp(cmd, "install") == 0) {
        char why[PKG_WHY_MAX];
        return pkg_install(o.root, o.args[1], o.force, why) ? 0 : cli_fail(why);
    }
    if (strcmp(cmd, "remove") == 0) {
        return cmd_remove(&o);
    }
    if (strcmp(cmd, "list") == 0) {
        return cmd_list(&o);
    }
    return strcmp(cmd, "verify") == 0 ? cmd_verify(&o) : cmd_role(&o);
}
