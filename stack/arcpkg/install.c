/*
 * install.c — arcpkg install (arcpkg.h; ADR-0010).
 *
 * 1. Read manifest.json (the first member, at most 1 MiB) and check it; a version already installed
 *    is a no-op. Check requires.packages against the database and, for a model, RAM.
 * 2. Extract every files/ member into <install_root>/.tmp-<name>-<version>/, hashing while writing;
 *    each must be listed in the manifest, once, with its size and SHA-256. Nothing else may be in the
 *    archive, and nothing listed may be missing. Each file is fsynced, then each directory holding one.
 * 3. Rename the temporary directory to <install_root>/<version>/, fsync install_root, record the
 *    version in the database. Programs and packs then lose their older versions; models keep them.
 * Any failure removes the temporary directory and leaves the database as it was.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arcpkg.h"

#define COPY_BUF (64u * 1024u)

typedef struct {
    const char *root;
    pkg_manifest m;
    pkg_stream *stream;
    char tmp[4096];  /* <root><install_root>/.tmp-<name>-<version> */
    char dest[4096]; /* <root><install_root>/<version> */
    char area[4096]; /* <root><install_root> */
    char top[4096];  /* <root>/<apps|models|packs> */
    bool made_area;  /* this install created area (and made_top: top), so a failure removes them */
    bool made_top;
    unsigned char *buf;
} installer;

bool pkg_version_dir(const char *root, const char *install_root, const char *version, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "%s%s/%s", root, install_root, version);
    return n >= 0 && (size_t)n < cap;
}

/* read_package_manifest: the first member, manifest.json, parsed and checked into in->m. */
static bool read_package_manifest(installer *in, char *why) {
    tar_member mem;
    int r = pkg_tar_next(in->stream, &mem, why);
    if (r <= 0) {
        return r == 0 ? pkg_fail(why, "the package is empty") : false;
    }
    if (mem.type != TAR_FILE || strcmp(mem.name, "manifest.json") != 0 || mem.size > PKG_MANIFEST_MAX) {
        return pkg_fail(why, "the first member must be manifest.json, at most %u bytes", PKG_MANIFEST_MAX);
    }
    char *text = malloc(mem.size + 1);
    size_t got = 0;
    for (long n = 1; text != NULL && n > 0 && got < mem.size; got += (size_t)(n > 0 ? n : 0)) {
        n = pkg_tar_read(in->stream, text + got, mem.size - got, why);
        if (n < 0) {
            free(text);
            return false;
        }
    }
    bool ok = text != NULL ? pkg_manifest_parse(&in->m, text, got, why) : pkg_fail(why, "out of memory");
    free(text); /* the parse copied what it keeps into its arena */
    return ok;
}

/* requires_ok: every package in requires.packages is installed, at min_version or later. */
static bool requires_ok(installer *in, char *why) {
    for (const arcjson_value *p = in->m.packages ? in->m.packages->u.children.first : NULL; p != NULL; p = p->next) {
        const char *name = arcjson_string(arcjson_get(p, "name"));
        const char *min = arcjson_string(arcjson_get(p, "min_version"));
        pkg_db db;
        bool ok = pkg_db_load(in->root, name, &db, why);
        bool met = ok && db.n_versions > 0 &&
                   (min == NULL || arcjson_schema_version_cmp(db.versions[db.n_versions - 1], min) >= 0);
        pkg_db_free(&db);
        if (!ok) {
            return false;
        }
        if (!met) {
            return pkg_fail(why, "needs package %s%s%s installed first", name, min ? " " : "", min ? min : "");
        }
    }
    return true;
}

/* parent_dirs: create the directories leading to tmp/rel (rel's own last part excluded). */
static bool parent_dirs(installer *in, const char *rel, char *why) {
    char path[4096];
    const char *slash = strrchr(rel, '/');
    if (slash == NULL) {
        return true;
    }
    if (snprintf(path, sizeof(path), "%s/%.*s", in->tmp, (int)(slash - rel), rel) >= (int)sizeof(path) ||
        !arcutil_make_dirs(path)) {
        return pkg_fail(why, "%s: %s", rel, strerror(errno));
    }
    return true;
}

/* extract_file: one files/ member into tmp, hashed and checked against its manifest entry. */
static bool extract_file(installer *in, const tar_member *mem, const char *rel, char *why) {
    pkg_file *f = pkg_manifest_find(&in->m, rel, strlen(rel));
    if (f == NULL || f->seen) {
        return pkg_fail(why, f ? "'%s' is in the archive twice" : "'%s' is in the archive but not in manifest.json",
                        rel);
    }
    f->seen = true;
    if (mem->size != f->size) {
        return pkg_fail(why, "'%s' is %llu bytes, manifest.json says %llu", rel, (unsigned long long)mem->size,
                        (unsigned long long)f->size);
    }
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/%s", in->tmp, rel) >= (int)sizeof(path) || !parent_dirs(in, rel, why)) {
        return pkg_fail(why, "'%s': path too long or no directory", rel);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mem->mode & 0111) ? 0755 : 0644);
    if (fd < 0) {
        return pkg_fail(why, "%s: %s", rel, strerror(errno));
    }
    arcutil_sha256 sha;
    arcutil_sha256_init(&sha);
    bool ok = true;
    for (long n; ok && (n = pkg_tar_read(in->stream, in->buf, COPY_BUF, why)) != 0;) {
        ok = n > 0;
        for (long done = 0; ok && done < n;) {
            ssize_t w = write(fd, in->buf + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            ok = w > 0 || pkg_fail(why, "writing %s: %s", rel, w < 0 ? strerror(errno) : "short write");
            done += w > 0 ? w : 0;
        }
        if (ok) {
            arcutil_sha256_update(&sha, in->buf, (size_t)n);
        }
    }
    if (ok && fsync(fd) != 0) {
        ok = pkg_fail(why, "writing %s: %s", rel, strerror(errno));
    }
    if (close(fd) != 0 && ok) {
        ok = pkg_fail(why, "writing %s: %s", rel, strerror(errno));
    }
    unsigned char digest[ARCUTIL_SHA256_SIZE];
    char hex[ARCUTIL_SHA256_HEX];
    arcutil_sha256_final(&sha, digest);
    arcutil_sha256_hex(digest, hex);
    return ok && (strcmp(hex, f->sha256) == 0 || pkg_fail(why, "'%s': SHA-256 differs from manifest.json", rel));
}

/* extract: every member after manifest.json; then nothing listed may be missing. */
static bool extract(installer *in, char *why) {
    tar_member mem;
    int r;
    while ((r = pkg_tar_next(in->stream, &mem, why)) > 0) {
        if (mem.type == TAR_DIR && (strcmp(mem.name, "files") == 0 || strncmp(mem.name, "files/", 6) == 0)) {
            char path[4096];
            if (mem.name[5] == '/' &&
                (snprintf(path, sizeof(path), "%s/%s", in->tmp, mem.name + 6) >= (int)sizeof(path) ||
                 !arcutil_make_dirs(path))) {
                return pkg_fail(why, "'%s': %s", mem.name, strerror(errno));
            }
        } else if (mem.type == TAR_FILE && strncmp(mem.name, "files/", 6) == 0) {
            if (!extract_file(in, &mem, mem.name + 6, why)) {
                return false;
            }
        } else {
            return pkg_fail(why, "'%s' is in the archive but outside files/", mem.name);
        }
    }
    if (r < 0) {
        return false;
    }
    for (size_t i = 0; i < in->m.n_files; i++) {
        if (!in->m.files[i].seen) {
            return pkg_fail(why, "'%s' is in manifest.json but not in the archive", in->m.files[i].path);
        }
    }
    return true;
}

/* shared_dirs: length of the leading directories rel[0..len) and prev[0..prev_len) have in common. */
static size_t shared_dirs(const char *rel, size_t len, const char *prev, size_t prev_len) {
    size_t common = 0;
    for (size_t k = 0; k <= len && k <= prev_len; k++) {
        if ((k == len || rel[k] == '/') && (k == prev_len || prev[k] == '/')) {
            common = k;
        }
        if (k == len || k == prev_len || rel[k] != prev[k]) {
            break;
        }
    }
    return common;
}

/*
 * sync_dirs: fsync every directory in tmp that holds a file or a directory, each once, then tmp.
 * Files are sorted, so a directory's files are contiguous: a directory is synced when the walk first
 * enters it (the parts of a file's directory beyond what it shares with the previous file's).
 */
static bool sync_dirs(installer *in, char *why) {
    const char *prev = "";
    size_t prev_len = 0;
    for (size_t i = 0; i < in->m.n_files; i++) {
        const char *rel = in->m.files[i].path;
        const char *slash = strrchr(rel, '/');
        size_t len = slash ? (size_t)(slash - rel) : 0;
        for (size_t k = shared_dirs(rel, len, prev, prev_len) + 1; k <= len; k++) {
            char dir[4096];
            if ((k == len || rel[k] == '/') &&
                (snprintf(dir, sizeof(dir), "%s/%.*s", in->tmp, (int)k, rel) >= (int)sizeof(dir) ||
                 !arcutil_sync_dir(dir))) {
                return pkg_fail(why, "%s: %s", dir, strerror(errno));
            }
        }
        prev = rel;
        prev_len = len;
    }
    return arcutil_sync_dir(in->tmp) || pkg_fail(why, "%s: %s", in->tmp, strerror(errno));
}

/* fits_ram: a model's ram_mb plus the reserve fits the machine; with force, only a warning. */
static bool fits_ram(installer *in, bool force, char *why) {
    uint64_t ram = arcutil_ram_mib();
    if (in->m.kind != PKG_MODEL || in->m.ram_mb + PKG_RAM_RESERVE_MIB <= ram) {
        return true;
    }
    if (force) {
        fprintf(stderr, "arcpkg: warning: %s needs %llu MiB + %u MiB reserve, this machine has %llu MiB (--force)\n",
                in->m.name, (unsigned long long)in->m.ram_mb, PKG_RAM_RESERVE_MIB, (unsigned long long)ram);
        return true;
    }
    return pkg_fail(why,
                    "%s needs %llu MiB of RAM + %u MiB reserve; this machine has %llu MiB (--force to install anyway)",
                    in->m.name, (unsigned long long)in->m.ram_mb, PKG_RAM_RESERVE_MIB, (unsigned long long)ram);
}

/* drop_older: programs and packs keep only the newest version (lane-l.md); remove the others, oldest first. */
static bool drop_older(installer *in, char *why) {
    for (;;) {
        pkg_db db;
        bool ok = pkg_db_load(in->root, in->m.name, &db, why);
        const char *old = ok && db.n_versions > 0 && arcjson_schema_version_cmp(db.versions[0], in->m.version) < 0
                              ? db.versions[0]
                              : NULL;
        char dir[4096];
        if (old != NULL) {
            ok = (pkg_version_dir(in->root, in->m.install_root, old, dir, sizeof(dir)) && arcutil_remove_tree(dir)) ||
                 pkg_fail(why, "removing %s %s: %s", in->m.name, old, strerror(errno));
            ok = ok && pkg_db_save(in->root, &db, NULL, old, why);
        }
        pkg_db_free(&db);
        if (!ok || old == NULL) {
            return ok;
        }
    }
}

/* place: rename tmp into <version>/ and record it. */
static bool place(installer *in, pkg_db *db, char *why) {
    struct stat st;
    if (lstat(in->dest, &st) == 0 && !arcutil_remove_tree(in->dest)) { /* left by an install cut short after rename */
        return pkg_fail(why, "%s: %s", in->dest, strerror(errno));
    }
    if (rename(in->tmp, in->dest) != 0 || !arcutil_sync_dir(in->area) ||
        (in->made_area && !arcutil_sync_dir(in->top)) ||
        (in->made_top && !arcutil_sync_dir(in->root[0] ? in->root : "/"))) {
        return pkg_fail(why, "%s: %s", in->dest, strerror(errno));
    }
    if (!pkg_db_save(in->root, db, in->m.doc, NULL, why)) {
        arcutil_remove_tree(in->dest);
        return false;
    }
    return true;
}

/* run: steps 1-3 for an open stream; the caller cleans up. */
static bool run(installer *in, bool force, bool *done, char *why) {
    pkg_db db;
    if (!read_package_manifest(in, why)) {
        return false;
    }
    if (!pkg_db_load(in->root, in->m.name, &db, why)) {
        pkg_db_free(&db);
        return false;
    }
    if (pkg_db_find(&db, in->m.version) >= 0) {
        printf("%s %s is already installed\n", in->m.name, in->m.version);
        pkg_db_free(&db);
        *done = true;
        return true;
    }
    const char *newest = db.n_versions > 0 ? db.versions[db.n_versions - 1] : NULL;
    bool ok = in->m.kind == PKG_MODEL || newest == NULL || arcjson_schema_version_cmp(newest, in->m.version) < 0 ||
              pkg_fail(why, "%s %s is installed, which is newer; remove it first", in->m.name, newest);
    ok = ok && requires_ok(in, why) && fits_ram(in, force, why);
    ok = ok && (snprintf(in->area, sizeof(in->area), "%s%s", in->root, in->m.install_root) < (int)sizeof(in->area)) &&
         (snprintf(in->tmp, sizeof(in->tmp), "%s/.tmp-%s-%s", in->area, in->m.name, in->m.version) <
          (int)sizeof(in->tmp)) &&
         pkg_version_dir(in->root, in->m.install_root, in->m.version, in->dest, sizeof(in->dest));
    if (ok) {
        snprintf(in->top, sizeof(in->top), "%s/%s", in->root, pkg_area_name(in->m.kind));
        in->made_top = access(in->top, F_OK) != 0;
        in->made_area = access(in->area, F_OK) != 0;
    }
    if (ok && (!arcutil_make_dirs(in->area) || !arcutil_remove_tree(in->tmp) || mkdir(in->tmp, 0755) != 0)) {
        ok = pkg_fail(why, "%s: %s", in->tmp, strerror(errno));
    }
    ok = ok && extract(in, why) && sync_dirs(in, why) && place(in, &db, why);
    pkg_db_free(&db);
    if (ok && in->m.kind != PKG_MODEL) {
        ok = drop_older(in, why);
    }
    return ok;
}

bool pkg_install(const char *root, const char *path, bool force, char *why) {
    installer in = {.root = root};
    in.buf = malloc(COPY_BUF);
    in.stream = in.buf ? pkg_stream_open(path, why) : NULL;
    bool done = false;
    bool ok = in.stream != NULL && run(&in, force, &done, why);
    if (in.buf == NULL) {
        ok = pkg_fail(why, "out of memory");
    }
    if (!ok && in.tmp[0] != '\0') { /* no trace of a failed install */
        arcutil_remove_tree(in.tmp);
        if (in.made_area) {
            rmdir(in.area);
        }
        if (in.made_top) {
            rmdir(in.top);
        }
    }
    if (ok && !done) {
        printf("installed %s %s (%s, %zu files)\n", in.m.name, in.m.version, pkg_kind_name(in.m.kind), in.m.n_files);
    }
    pkg_stream_close(in.stream);
    pkg_manifest_free(&in.m);
    free(in.buf);
    return ok;
}
