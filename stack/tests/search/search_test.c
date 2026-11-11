/*
 * search_test — the program search_lane.py drives for lane.search (M3-06a).
 *
 *   search_test write DIR NAME VERSION SET   write a small pack (SET: medical, old, outdoors, outdoors-more)
 *   search_test index DIR                     arcsearch_build_fts(DIR)
 *   search_test api ROOT                      library checks: MATCH expressions, open, search edge cases
 *
 * Each document's chunks are its paragraphs, in order: the text is the paragraphs joined.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lane_check.h"
#include "arcsearch.h"

#define MAX_PARAS 4

typedef struct {
    const char *title;
    const char *paras[MAX_PARAS];    /* NULL-terminated */
    const char *sections[MAX_PARAS]; /* one per paragraph, parts joined by \037 */
} doc;

/*
 * Written after every set's own documents, so each pack has enough chunks for bm25: in a tiny pack a
 * word found in half the chunks or more has an IDF of about 0 and every score is 0.
 */
static const doc FILLER[] = {
    {"Sprains", {"Rest, ice, compression and elevation for the first two days.\n"}, {"Sprains"}},
    {"Dehydration", {"Drink small amounts often; watch for dark urine.\n"}, {"Dehydration"}},
    {"Allergies", {"Antihistamines ease mild reactions; swelling of the throat is urgent.\n"}, {"Allergies"}},
    {"Fractures", {"Splint the limb as it lies and check the pulse beyond the break.\n"}, {"Fractures"}},
    {"Nosebleeds", {"Lean forward and pinch the soft part of the nose for ten minutes.\n"}, {"Nosebleeds"}},
    {"Stings", {"Scrape a bee sting out sideways; cold packs ease the swelling.\n"}, {"Stings"}},
    {NULL, {NULL}, {NULL}},
};

static const doc MEDICAL[] = {
    {"Hypothermia",
     {"\0011Hypothermia\nHypothermia is a drop in core body temperature below 35 \302\260C.\n",
      "\0012Signs\nShivering, confusion and slurred speech.\n",
      "\0012Treatment\nMove the person somewhere warm and replace wet clothing.\n"},
     {"Hypothermia", "Hypothermia\037Signs", "Hypothermia\037Treatment"}},
    {"Heat stroke",
     {"Heat stroke is overheating of the body; it is not like burns or hypothermia.\n"},
     {"Heat stroke"}},
    {"Burns", {"Cool a burn under running water for twenty minutes.\n"}, {"Burns"}},
    {NULL, {NULL}, {NULL}},
};

/* An older version of the medical pack: its words must never be found while 1.0.0 is installed. */
static const doc OLD[] = {
    {"Frostbite", {"An old edition about frostbite and hypothermia.\n"}, {"Frostbite"}},
    {NULL, {NULL}, {NULL}},
};

static const doc OUTDOORS[] = {
    {"Campfire safety",
     {"Keep water and a shovel near the fire. Most burns at camp come from embers.\n"},
     {"Campfire"}},
    {"Glacier travel",
     {"Rope up before crossing crevasses on the glacier above Z\303\274rich.\n"},
     {"Glacier\037Ropes"}},
    {NULL, {NULL}, {NULL}},
};

/* outdoors with one more document: a rebuilt pack whose old index is stale. */
static const doc OUTDOORS_MORE[] = {
    {"Campfire safety",
     {"Keep water and a shovel near the fire. Most burns at camp come from embers.\n"},
     {"Campfire"}},
    {"Glacier travel",
     {"Rope up before crossing crevasses on the glacier above Z\303\274rich.\n"},
     {"Glacier\037Ropes"}},
    {"Knots", {"A bowline makes a fixed loop at the end of a rope.\n"}, {"Knots"}},
    {NULL, {NULL}, {NULL}},
};

/* add_docs: add docs[] (up to the NULL title), each paragraph one chunk. */
static bool add_docs(arcpack_writer *w, const doc *docs, const char **why) {
    bool ok = true;
    for (const doc *d = docs; ok && d->title != NULL; d++) {
        char text[1024];
        arcpack_chunk_spec chunks[MAX_PARAS];
        size_t len = 0, n = 0;
        for (; n < MAX_PARAS && d->paras[n] != NULL; n++) {
            size_t plen = strlen(d->paras[n]);
            memcpy(text + len, d->paras[n], plen);
            chunks[n] = (arcpack_chunk_spec){(uint32_t)len, (uint32_t)plen, d->sections[n], strlen(d->sections[n]), 1};
            len += plen;
        }
        ok = arcpack_writer_add_doc(w, d->title, strlen(d->title), text, len, chunks, n, why);
    }
    return ok;
}

static int write_pack(const char *dir, const char *name, const char *version, const char *set) {
    const doc *docs = strcmp(set, "medical") == 0         ? MEDICAL
                      : strcmp(set, "old") == 0           ? OLD
                      : strcmp(set, "outdoors") == 0      ? OUTDOORS
                      : strcmp(set, "outdoors-more") == 0 ? OUTDOORS_MORE
                                                          : NULL;
    const char *why = "unknown set";
    arcpack_writer *w = docs ? arcpack_writer_open(dir, name, version, &why) : NULL;
    bool ok = w != NULL && add_docs(w, docs, &why) && add_docs(w, FILLER, &why);
    if (ok) {
        ok = arcpack_writer_finish(w, &why);
    } else {
        arcpack_writer_abort(w);
    }
    if (!ok) {
        fprintf(stderr, "write %s: %s\n", dir, why);
    }
    return ok ? 0 : 1;
}

/* expr_is: the MATCH expression for `query` is `want`. */
static bool expr_is(const char *query, const char *want) {
    char buf[256];
    size_t n = arcsearch_match_expr(query, buf, sizeof(buf));
    if (n != strlen(want) || strcmp(buf, want) != 0) {
        printf("query '%s': expression '%s', expected '%s'\n", query, buf, want);
        return false;
    }
    return true;
}

static void check_expressions(void) {
    lane_check(expr_is("What is Hypothermia?", "\"hypothermia\""), "stopwords dropped, ASCII lowercased");
    lane_check(expr_is("NEAR(water fire) AND -smoke*", "\"near\" OR \"water\" OR \"fire\" OR \"smoke\""),
               "FTS5 syntax becomes words");
    lane_check(expr_is("\"title\": {body} ^x", "\"title\" OR \"body\" OR \"x\""), "quotes, columns, ^ become words");
    lane_check(expr_is("", "") && expr_is("the of and is", "") && expr_is("\"*-()", ""), "no words: empty");
    lane_check(
        expr_is("Z\303\274rich caf\303\251, na\303\257ve", "\"z\303\274rich\" OR \"caf\303\251\" OR \"na\303\257ve\""),
        "Unicode letters stay in their words");
    lane_check(expr_is("e\314\201t\303\251 \xff water", "\"e\314\201t\303\251\" OR \"water\""),
               "combining accent kept, bad UTF-8 is a separator");
    char small[10];
    lane_check(arcsearch_match_expr("water fire", small, sizeof(small)) == 7 && strcmp(small, "\"water\"") == 0,
               "words that don't fit are dropped");
    static char longq[20000];
    for (size_t i = 0; i + 6 < sizeof(longq); i += 6) {
        memcpy(longq + i, "water ", 6);
    }
    char big[4096];
    size_t n = arcsearch_match_expr(longq, big, sizeof(big));
    lane_check(n > 0 && n < sizeof(big) && big[n - 1] == '"', "a long query is cut at a word");
}

static int api(const char *root) {
    check_expressions();
    const char *why;
    arcsearch *s = arcsearch_open(root, &why);
    lane_check(s != NULL, s ? "open the packs" : why);
    if (s == NULL) {
        return lane_check_done("search-api");
    }
    lane_check(arcsearch_pack_count(s) == 2 && strcmp(arcsearch_pack_name(s, 0), "medical") == 0 &&
                   strcmp(arcsearch_pack_name(s, 1), "outdoors") == 0 && arcsearch_warning_count(s) == 0,
               "two packs, in name order, no warnings");
    arcsearch_result r[4];
    size_t n = 9;
    lane_check(arcsearch_search(s, "water", NULL, 1, r, &n, &why) && n == 1, "k = 1 gives one result");
    lane_check(arcsearch_search(s, "water", NULL, 0, r, &n, &why) && n == 0, "k = 0 gives none");
    lane_check(arcsearch_search(s, "frostbite", NULL, 4, r, &n, &why) && n == 0, "an older version is not searched");
    lane_check(arcsearch_search(s, "hypothermia", "outdoors", 4, r, &n, &why) && n == 0, "a pack filter limits it");
    lane_check(!arcsearch_search(s, "water", "nope", 4, r, &n, &why) && strstr(why, "no installed pack named 'nope'"),
               "an unknown pack is an error");
    lane_check(arcsearch_search(s, "rope bowline", NULL, 4, r, &n, &why) && n == 1 &&
                   strcmp(r[0].id, "outdoors:1:0") == 0 && r[0].section_length == 13,
               "result fields: ID, section");
    arcsearch_close(s);
    s = arcsearch_open("/nonexistent-arc-root", &why);
    lane_check(s != NULL && arcsearch_pack_count(s) == 0 && arcsearch_warning_count(s) == 1,
               "no packs directory: no packs, one warning");
    arcsearch_close(s);
    return lane_check_done("search-api");
}

int main(int argc, char **argv) {
    if (argc == 6 && strcmp(argv[1], "write") == 0) {
        return write_pack(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 3 && strcmp(argv[1], "index") == 0) {
        const char *why;
        if (!arcsearch_build_fts(argv[2], &why)) {
            fprintf(stderr, "index: %s\n", why);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "api") == 0) {
        return api(argv[2]);
    }
    fprintf(stderr, "usage: search_test write DIR NAME VERSION SET | index DIR | api ROOT\n");
    return 2;
}
