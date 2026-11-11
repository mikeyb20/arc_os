/*
 * arcsearch_build.c — writes a pack's FTS5 index, fts/index.sqlite (arcsearch.h; schema in
 * docs/formats/pack-v1.md). Runs where packs are built (tests now, the pipeline in M3-04).
 *
 * The index is written to fts/index.sqlite.part with no journal (a failed build is simply
 * deleted), in one transaction, merged into one segment ('optimize'), fsynced and renamed into
 * place. Memory: two buffers the size of the largest document's text, and SQLite's cache.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arcutil.h"
#include "arcsearch.h"

static const char SCHEMA[] = "CREATE VIRTUAL TABLE fts USING fts5(title, section, body, content='',"
                             " tokenize='porter unicode61 remove_diacritics 2');"
                             "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);";

typedef struct {
    arcpack *pack;
    sqlite3 *db;
    sqlite3_stmt *insert;
    char *text;  /* the current document's marked-up text */
    char *plain; /* one chunk's plain text */
    size_t cap;  /* bytes in each buffer */
} builder;

static _Thread_local char build_why[512];

/* build_fail: format *why and return false. */
__attribute__((format(printf, 2, 3))) static bool build_fail(const char **why, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(build_why, sizeof(build_why), fmt, ap);
    va_end(ap);
    *why = build_why;
    return false;
}

/* sql_fail: an SQLite error on the index being built. */
static bool sql_fail(builder *b, const char *what, const char **why) {
    return build_fail(why, "fts/index.sqlite: %s: %s", what, sqlite3_errmsg(b->db));
}

/* put_meta: one row of the meta table. */
static bool put_meta(builder *b, const char *key, const char *value, const char **why) {
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(b->db, "INSERT INTO meta(key, value) VALUES(?1, ?2)", -1, &st, NULL) == SQLITE_OK &&
              sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok || sql_fail(b, "meta", why);
}

/* grow: make both buffers hold at least `need` bytes. */
static bool grow(builder *b, size_t need, const char **why) {
    if (need <= b->cap) {
        return true;
    }
    char *t = realloc(b->text, need);
    if (t != NULL) {
        b->text = t;
    }
    char *p = t != NULL ? realloc(b->plain, need) : NULL;
    if (p != NULL) {
        b->plain = p;
        b->cap = need;
    }
    return p != NULL || build_fail(why, "out of memory for a %zu-byte document", need);
}

/* add_chunk: one row of the fts table: chunk `index` of the document whose text is in b->text. */
static bool add_chunk(builder *b, uint32_t index, const arcpack_doc *doc, const char **why) {
    arcpack_chunk c;
    if (!arcpack_chunk_get(b->pack, index, &c)) {
        return build_fail(why, "%s", arcpack_error(b->pack));
    }
    size_t body = arcpack_plain(b->plain, b->text + c.offset, c.length);
    sqlite3_stmt *st = b->insert;
    bool ok = sqlite3_bind_int64(st, 1, index) == SQLITE_OK &&
              sqlite3_bind_text(st, 2, doc->title, (int)doc->title_length, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_text(st, 3, c.section, (int)c.section_length, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_text(st, 4, b->plain, (int)body, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE;
    sqlite3_reset(st);
    return ok || sql_fail(b, "insert", why);
}

/* fill_index: the meta rows and one fts row per chunk, document by document. */
static bool fill_index(builder *b, const char **why) {
    char count[16];
    snprintf(count, sizeof(count), "%u", arcpack_chunk_count(b->pack));
    if (!put_meta(b, "schema_version", "1", why) || !put_meta(b, "pack_name", arcpack_name(b->pack), why) ||
        !put_meta(b, "pack_version", arcpack_version(b->pack), why) || !put_meta(b, "chunk_count", count, why)) {
        return false;
    }
    if (sqlite3_prepare_v2(b->db, "INSERT INTO fts(rowid, title, section, body) VALUES(?1, ?2, ?3, ?4)", -1, &b->insert,
                           NULL) != SQLITE_OK) {
        return sql_fail(b, "prepare", why);
    }
    for (uint32_t d = 0; d < arcpack_doc_count(b->pack); d++) {
        arcpack_doc doc;
        size_t len;
        if (!arcpack_doc_get(b->pack, d, &doc)) {
            return build_fail(why, "%s", arcpack_error(b->pack));
        }
        if (doc.chunk_count == 0) {
            continue;
        }
        if (!grow(b, doc.text_size, why)) {
            return false;
        }
        if (!arcpack_doc_text(b->pack, d, b->text, b->cap, &len)) {
            return build_fail(why, "%s", arcpack_error(b->pack));
        }
        for (uint32_t i = 0; i < doc.chunk_count; i++) {
            if (!add_chunk(b, doc.first_chunk + i, &doc, why)) {
                return false;
            }
        }
    }
    return true;
}

/* sync_file: fsync a closed file by name. */
static bool sync_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

bool arcsearch_build_fts(const char *pack_dir, const char **why) {
    const char *err;
    builder b = {0};
    b.pack = arcpack_open(pack_dir, &err);
    if (b.pack == NULL) {
        return build_fail(why, "%s", err);
    }
    char dir[PATH_MAX];
    char part[PATH_MAX];
    char path[PATH_MAX];
    bool ok = snprintf(dir, sizeof(dir), "%s/fts", pack_dir) < (int)sizeof(dir) &&
              snprintf(part, sizeof(part), "%s/index.sqlite.part", dir) < (int)sizeof(part) &&
              snprintf(path, sizeof(path), "%s/index.sqlite", dir) < (int)sizeof(path);
    if (!ok) {
        arcpack_close(b.pack);
        return build_fail(why, "%s: path too long", pack_dir);
    }
    if (!arcutil_make_dir(dir) || (unlink(part) != 0 && errno != ENOENT)) {
        ok = build_fail(why, "%s: %s", dir, strerror(errno));
    } else if (sqlite3_open_v2(part, &b.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        ok = sql_fail(&b, "open", why);
    } else if (sqlite3_exec(b.db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;", NULL, NULL, NULL) != SQLITE_OK ||
               sqlite3_exec(b.db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK ||
               sqlite3_exec(b.db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        ok = sql_fail(&b, "schema", why);
    } else {
        ok = fill_index(&b, why);
        if (ok && (sqlite3_exec(b.db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK ||
                   sqlite3_exec(b.db, "INSERT INTO fts(fts) VALUES('optimize')", NULL, NULL, NULL) != SQLITE_OK)) {
            ok = sql_fail(&b, "commit", why);
        }
    }
    sqlite3_finalize(b.insert);
    if (sqlite3_close(b.db) != SQLITE_OK && ok) {
        ok = build_fail(why, "fts/index.sqlite: close failed");
    }
    if (ok && (!sync_file(part) || rename(part, path) != 0 || !arcutil_sync_dir(dir))) {
        ok = build_fail(why, "%s: %s", path, strerror(errno));
    }
    if (!ok) {
        unlink(part);
    }
    free(b.text);
    free(b.plain);
    arcpack_close(b.pack);
    return ok;
}
