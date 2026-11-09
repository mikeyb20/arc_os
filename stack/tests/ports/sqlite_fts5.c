/*
 * lane.sqlite_fts5 (M2-02): SQLite FTS5 the way packs use it (ADR-0011, M3-06a).
 *
 * Builds a small FTS5 table with the pack tokenizer, checks bm25() ordering and porter
 * stemming, then reopens the file read-only as file:...?immutable=1 (no locks, no
 * journal: how installed packs are opened), runs the same query again and checks that a
 * write is refused.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "../lane_check.h"

#define DB_FILE  "sqlite_fts5_test.db"
#define DB_URI   "file:" DB_FILE "?immutable=1"
#define MAX_HITS 8

static const char SETUP[] = "CREATE VIRTUAL TABLE docs USING fts5(title, body,"
                            " tokenize = 'porter unicode61 remove_diacritics 2');"
                            "INSERT INTO docs(rowid, title, body) VALUES"
                            " (1, 'Water', 'Boil water for one minute to make the water safe to drink.'),"
                            " (2, 'Hypothermia', 'Body temperature below 35 C. Warm the person slowly with blankets,"
                            " skin contact and warm sweet drinks; give water only if awake.'),"
                            " (3, 'Fire', 'Keep the fire small and out of the wind.'),"
                            " (4, 'Wells', 'Wells reach ground water.');";

/* query: rowids matching `match`, best bm25() first; returns the count, -1 on error. */
static int query(sqlite3 *db, const char *match, long long *rowids) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT rowid FROM docs WHERE docs MATCH ?1 ORDER BY bm25(docs)", -1, &st, NULL) !=
        SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(st, 1, match, -1, SQLITE_STATIC);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < MAX_HITS) {
        rowids[n++] = sqlite3_column_int64(st, 0);
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        fprintf(stderr, "step: %s\n", sqlite3_errmsg(db));
        n = -1;
    }
    sqlite3_finalize(st);
    return n;
}

/* check_ranking: 'water' ranks the most hits first, then the shorter document. */
static void check_ranking(sqlite3 *db, const char *when) {
    static const long long expect[] = {1, 4, 2};
    long long got[MAX_HITS];
    char what[96];
    int n = query(db, "water", got);
    snprintf(what, sizeof(what), "%s: 'water' ordered by bm25() is 1, 4, 2", when);
    lane_check(n == 3 && memcmp(got, expect, sizeof(expect)) == 0, what);
    n = query(db, "boiling", got);
    snprintf(what, sizeof(what), "%s: porter stemming finds 'boil' for 'boiling'", when);
    lane_check(n == 1 && got[0] == 1, what);
}

int main(void) {
    sqlite3 *db = NULL;
    unlink(DB_FILE);
    unlink(DB_FILE "-journal");

    lane_check(sqlite3_open(DB_FILE, &db) == SQLITE_OK, "create the database");
    lane_check(sqlite3_exec(db, SETUP, NULL, NULL, NULL) == SQLITE_OK, "create and fill the FTS5 table");
    check_ranking(db, "read-write");
    lane_check(sqlite3_close(db) == SQLITE_OK, "close");

    db = NULL;
    lane_check(sqlite3_open_v2(DB_URI, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) == SQLITE_OK,
               "reopen as " DB_URI);
    check_ranking(db, "immutable");
    int rc = sqlite3_exec(db, "INSERT INTO docs(title, body) VALUES ('x', 'water')", NULL, NULL, NULL);
    lane_check(rc == SQLITE_READONLY, "a write to the immutable database is refused");
    lane_check(sqlite3_close(db) == SQLITE_OK, "close");

    return lane_check_done("sqlite_fts5");
}
