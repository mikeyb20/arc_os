/*
 * arcpkg.h — the parts of the arcpkg tool (M5-02, ADR-0010) and what they share.
 *
 *   tar.c       a .arcpkg read as a stream: zstd decompression (window at most 8 MiB) under a ustar
 *               reader that accepts only regular files and directories with safe relative names
 *   manifest.c  manifest.json of a package: parsed with arcjson, checked against the rules a reader
 *               must enforce (schemas/arcpkg.schema.json, schemas/README.md)
 *   db.c        the package database, <root>/var/lib/arcpkg/<name>.json, and <root>/models/roles.json
 *   install.c   install: extract into <install_root>/.tmp-<name>-<version>/ hashing while writing,
 *               then rename into <install_root>/<version>/ and record it
 *   main.c      the commands (install, remove, list, verify, role) and the leftover cleanup
 *
 * Memory does not grow with the package: a 128 KiB input and output buffer, the zstd window (at
 * most 8 MiB), one 64 KiB copy buffer, and the manifest (at most 1 MiB) with its parse.
 */
#ifndef ARCHOS_STACK_ARCPKG_H
#define ARCHOS_STACK_ARCPKG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arcjson.h"
#include "arcutil.h"

#define PKG_MANIFEST_MAX    (1u << 20) /* manifest.json in a package */
#define PKG_DB_MAX          (64u << 20) /* one package's database file (a big pack lists many files) */
#define PKG_RAM_RESERVE_MIB 1024u       /* RAM kept back for the OS in the model fit check (as arc-llm) */
#define PKG_ZSTD_WINDOW_LOG 23          /* 8 MiB: the largest window a package may need (schemas/README.md) */
#define PKG_WHY_MAX         512

/* Every command reports failures as text in a caller buffer of PKG_WHY_MAX bytes. */
__attribute__((format(printf, 2, 3))) bool pkg_fail(char *why, const char *fmt, ...);

/* ---- tar.c ---------------------------------------------------------------------------------------- */

typedef struct pkg_stream pkg_stream;

/* pkg_stream_open: start reading the .arcpkg at `path`; NULL with why on failure. */
pkg_stream *pkg_stream_open(const char *path, char *why);
void pkg_stream_close(pkg_stream *s);

typedef enum { TAR_FILE, TAR_DIR } tar_type;

typedef struct {
    char name[257]; /* prefix "/" name (155 + 1 + 100), trailing '/' removed; a safe relative path */
    tar_type type;
    uint64_t size;
    unsigned mode;
} tar_member;

/*
 * pkg_tar_next: the next member's header (any unread data of the previous member is skipped).
 * 1 = *m filled, 0 = end of archive, -1 = error in why (a link or other member type, an
 * absolute path, a `..` part, a bad checksum, a name over the ustar limits, a damaged stream).
 */
int pkg_tar_next(pkg_stream *s, tar_member *m, char *why);

/* pkg_tar_read: up to `cap` bytes of the current member's data; 0 at its end, -1 on error. */
long pkg_tar_read(pkg_stream *s, void *buf, size_t cap, char *why);

/* ---- manifest.c ----------------------------------------------------------------------------------- */

typedef enum { PKG_PROGRAM, PKG_MODEL, PKG_PACK } pkg_kind;

typedef struct {
    const char *path; /* relative to install_root/<version>/, NUL-terminated (in the manifest's arena) */
    uint64_t size;
    const char *sha256; /* 64 lowercase hex digits */
    bool seen;          /* install: the archive held it */
} pkg_file;

typedef struct {
    arcjson_arena arena;
    const arcjson_value *doc;
    pkg_kind kind;
    const char *name;
    const char *version;
    const char *install_root; /* "/apps/<name>", "/models/<name>" or "/packs/<name>" */
    uint64_t ram_mb;          /* the larger of requires.ram_mb and model.ram_mb */
    const arcjson_value *packages; /* requires.packages, or NULL */
    pkg_file *files;               /* sorted by path, no duplicates */
    size_t n_files;
    uint64_t size; /* sum of file sizes */
} pkg_manifest;

/* pkg_kind_name: "program", "model" or "pack". */
const char *pkg_kind_name(pkg_kind k);

/* pkg_area_name: the top directory of the kind's install_root: "apps", "models" or "packs". */
const char *pkg_area_name(pkg_kind k);

/* pkg_manifest_parse: parse and check text[0..len) into m (free with pkg_manifest_free even on failure). */
bool pkg_manifest_parse(pkg_manifest *m, const char *text, size_t len, char *why);

/* pkg_manifest_from: check an already parsed manifest (from the database) into m; m->arena is unused. */
bool pkg_manifest_from(pkg_manifest *m, const arcjson_value *doc, char *why);

void pkg_manifest_free(pkg_manifest *m);

/* pkg_manifest_find: the file entry for path[0..len), or NULL (O(log n)). */
pkg_file *pkg_manifest_find(const pkg_manifest *m, const char *path, size_t len);

/* ---- db.c ----------------------------------------------------------------------------------------- */

/* One package's database entry: its installed versions, each with the manifest it was installed from. */
typedef struct {
    arcjson_arena arena;
    char *text;
    char name[65];
    size_t n_versions;
    const char *versions[64];             /* sorted, lowest first */
    const arcjson_value *manifests[64];   /* manifest of each version */
} pkg_db;

#define PKG_DB_VERSIONS_MAX 64u

/* pkg_db_path: <root>/var/lib/arcpkg[/<name>.json] into buf. */
bool pkg_db_path(const char *root, const char *name, char *buf, size_t cap);

/* pkg_db_load: the entry for `name` into db; true with n_versions 0 if there is none. */
bool pkg_db_load(const char *root, const char *name, pkg_db *db, char *why);
void pkg_db_free(pkg_db *db);

/* pkg_db_find: index of `version` in db, or -1. */
int pkg_db_find(const pkg_db *db, const char *version);

/*
 * pkg_db_save: write the entry with db's versions, plus `add` (a manifest, or NULL) and minus
 * version `drop` (or NULL), durably; with no versions left the file is removed.
 */
bool pkg_db_save(const char *root, const pkg_db *db, const arcjson_value *add, const char *drop, char *why);

/* pkg_role_uses: some role in <root>/models/roles.json points at model name@version; *role names it. */
bool pkg_role_uses(const char *root, const char *name, const char *version, char *role, size_t cap);

/* pkg_roles_show: print roles.json's roles, one "role name version" per line. */
bool pkg_roles_show(const char *root, char *why);

/* pkg_roles_set: point `role` at installed model name@version, whose model.json must list the role. */
bool pkg_roles_set(const char *root, const char *role, const char *name, const char *version, char *why);

/* ---- install.c ------------------------------------------------------------------------------------ */

/* pkg_install: install the package file `path` under root (force: install a model that doesn't fit RAM). */
bool pkg_install(const char *root, const char *path, bool force, char *why);

/* pkg_version_dir: <root><install_root>/<version> into buf. */
bool pkg_version_dir(const char *root, const char *install_root, const char *version, char *buf, size_t cap);

#endif /* ARCHOS_STACK_ARCPKG_H */
