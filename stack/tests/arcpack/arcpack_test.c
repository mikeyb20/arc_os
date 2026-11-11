/*
 * arcpack_test (M3-02): the pack library, driven by arcpack_lane.py.
 *
 *   arcpack_test write3 DIR            write the three-document test pack (no manifest)
 *   arcpack_test read3 DIR SCRATCH     read it back and check everything; writer refusals into SCRATCH
 *   arcpack_test scan DIR              read every document, chunk and chunk ID; exit 1 at the first error
 *   arcpack_test big-write DIR N       a synthetic pack of N chunks, 20 per document
 *   arcpack_test big-lookup DIR        1000 random chunk lookups in a forked child; prints its peak RSS
 *
 * The driver writes each pack's manifest.json (the pipeline's job, M3-04) between write and read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../lane_check.h"
#include "arcpack.h"

#define NAME           "test-pack"
#define VERSION        "1.0.0"
#define BIG_DOC_CHUNKS 20
#define LOOKUPS        1000

/* ---- The three-document pack -------------------------------------------------------------------- */

/* Document 0: every kind of markup; its link names document 2. */
static const char DOC0[] = "\x01"
                           "1Hypothermia\n"
                           "Hypothermia is a body temperature below 35 \xC2\xB0"
                           "C. See \x02"
                           "2\x1F"
                           "frostbite\x03 too.\n"
                           "\x04hypothermia-stages\x1FThe stages of hypothermia\n"
                           "\x01"
                           "2Treatment\n"
                           "Move the person somewhere warm and dry.\n";
static const char DOC0_PLAIN[] = "Hypothermia\n"
                                 "Hypothermia is a body temperature below 35 \xC2\xB0"
                                 "C. See frostbite too.\n"
                                 "The stages of hypothermia\n"
                                 "Treatment\n"
                                 "Move the person somewhere warm and dry.\n";
/* Document 1: no chunks (a picture page, say). Document 2: two overlapping chunks, no headings. */
static const char DOC1[] = "\x04"
                           "cold-water\x1F\n";
static const char DOC2[] = "Frostbite damages skin and tissue \xE2\x80\x94 fingers, toes, nose.\n"
                           "Warm the area in water at 37\xE2\x80\x93"
                           "39 \xC2\xB0"
                           "C.\n";
static const char *const TITLES[] = {"Hypothermia", "Cold water", "Frostbite \xE2\x80\x94 first aid"};
static const char *const TEXTS[] = {DOC0, DOC1, DOC2};
static const size_t TEXT_LENS[] = {sizeof(DOC0) - 1, sizeof(DOC1) - 1, sizeof(DOC2) - 1};
static const char SECTION_TREATMENT[] = "Hypothermia\x1FTreatment";

/* offset_of: where `needle` starts in document d's text. */
static uint32_t offset_of(int d, const char *needle) {
    return (uint32_t)(strstr(TEXTS[d], needle) - TEXTS[d]);
}

/* The chunks of documents 0 and 2 (four in all, global 0..3). */
static void three_chunks(arcpack_chunk_spec c0[2], arcpack_chunk_spec c2[2]) {
    uint32_t image = offset_of(0, "\x04");
    c0[0] = (arcpack_chunk_spec){0, image, "Hypothermia", 11, 1u << 0};
    c0[1] = (arcpack_chunk_spec){image, (uint32_t)TEXT_LENS[0] - image, SECTION_TREATMENT,
                                 sizeof(SECTION_TREATMENT) - 1, (1u << 0) | (1u << 1)};
    uint32_t warm = offset_of(2, "Warm");
    c2[0] = (arcpack_chunk_spec){0, warm, NULL, 0, 1u << 2};
    c2[1] = (arcpack_chunk_spec){offset_of(2, "fingers"), (uint32_t)TEXT_LENS[2] - offset_of(2, "fingers"), NULL, 0,
                                 (1u << 1) | (1u << 2)};
}

static int write3(const char *dir) {
    const char *why;
    arcpack_writer *w = arcpack_writer_open(dir, NAME, VERSION, &why);
    if (w == NULL) {
        fprintf(stderr, "write3: %s\n", why);
        return 1;
    }
    arcpack_chunk_spec c0[2];
    arcpack_chunk_spec c2[2];
    three_chunks(c0, c2);
    const arcpack_chunk_spec *chunks[] = {c0, NULL, c2};
    const size_t counts[] = {2, 0, 2};
    for (int d = 0; d < 3; d++) {
        if (!arcpack_writer_add_doc(w, TITLES[d], strlen(TITLES[d]), TEXTS[d], TEXT_LENS[d], chunks[d], counts[d],
                                    &why)) {
            fprintf(stderr, "write3: document %d: %s\n", d, why);
            arcpack_writer_abort(w);
            return 1;
        }
    }
    if (!arcpack_writer_finish(w, &why)) {
        fprintf(stderr, "write3: %s\n", why);
        return 1;
    }
    printf("write3: 3 documents, 4 chunks\n");
    return 0;
}

/* ---- Reading it back ---------------------------------------------------------------------------- */

static bool same(const char *a, size_t a_len, const char *b, size_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static void check_docs(arcpack *p) {
    char buf[512];
    size_t len = 0;
    for (uint32_t d = 0; d < 3; d++) {
        arcpack_doc doc;
        bool ok = arcpack_doc_get(p, d, &doc) && same(doc.title, doc.title_length, TITLES[d], strlen(TITLES[d])) &&
                  doc.text_size == TEXT_LENS[d] && arcpack_doc_text(p, d, buf, sizeof(buf), &len) &&
                  same(buf, len, TEXTS[d], TEXT_LENS[d]);
        printf("document %u: %s\n", d, ok ? "title and text round-trip" : arcpack_error(p));
        lane_check(ok, "document title and text round-trip");
    }
    arcpack_doc doc;
    lane_check(arcpack_doc_get(p, 0, &doc) && doc.first_chunk == 0 && doc.chunk_count == 2 &&
                   arcpack_doc_get(p, 1, &doc) && doc.first_chunk == 2 && doc.chunk_count == 0 &&
                   arcpack_doc_get(p, 2, &doc) && doc.first_chunk == 2 && doc.chunk_count == 2,
               "first_chunk and chunk_count, a document without chunks");
    lane_check(!arcpack_doc_get(p, 3, &doc) && strstr(arcpack_error(p), "out of range") != NULL,
               "document past the end refused");
    lane_check(!arcpack_doc_text(p, 0, buf, TEXT_LENS[0] - 1, &len) && strstr(arcpack_error(p), "buffer") != NULL,
               "too small a buffer refused");
}

static void check_chunks(arcpack *p) {
    arcpack_chunk_spec c0[2];
    arcpack_chunk_spec c2[2];
    three_chunks(c0, c2);
    const arcpack_chunk_spec *want[] = {&c0[0], &c0[1], &c2[0], &c2[1]};
    const uint32_t want_doc[] = {0, 0, 2, 2};
    const char *const want_id[] = {NAME ":0:0", NAME ":0:1", NAME ":2:0", NAME ":2:1"};
    char buf[512];
    char id[ARCPACK_CHUNK_ID_MAX];
    for (uint32_t i = 0; i < 4; i++) {
        arcpack_chunk c;
        size_t len = 0;
        uint32_t back = 99;
        const arcpack_chunk_spec *w = want[i];
        bool ok = arcpack_chunk_get(p, i, &c) && c.document == want_doc[i] && c.number == i % 2 &&
                  c.offset == w->offset && c.length == w->length && c.tags == w->tags &&
                  same(c.section, c.section_length, w->section ? w->section : "", w->section_length) &&
                  arcpack_chunk_text(p, i, buf, sizeof(buf), &len) &&
                  same(buf, len, TEXTS[want_doc[i]] + w->offset, w->length) && arcpack_chunk_id(p, i, id, sizeof(id)) &&
                  strcmp(id, want_id[i]) == 0 && arcpack_chunk_parse(p, id, strlen(id), &back) && back == i;
        printf("chunk %u (%s): %s\n", i, want_id[i], ok ? "record, text and ID both ways" : arcpack_error(p));
        lane_check(ok, "chunk record, text, section, tags and ID both ways");
    }
    arcpack_chunk c;
    lane_check(!arcpack_chunk_get(p, 4, &c), "chunk past the end refused");

    static const char *const bad[] = {
        "other:0:0", NAME ":1:0", NAME ":3:0",   NAME ":0:2",  NAME ":0:01", NAME ":00:0",         NAME ":0:",
        NAME "::0",  NAME ":0",   NAME ":0:0:0", NAME ":-1:0", NAME ":0:+1", NAME ":4294967296:0", NAME ":0:4294967295",
        NAME,        "",
    };
    bool all_refused = true;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint32_t index;
        if (arcpack_chunk_parse(p, bad[i], strlen(bad[i]), &index)) {
            printf("accepted bad chunk ID '%s'\n", bad[i]);
            all_refused = false;
        }
    }
    lane_check(all_refused, "malformed, foreign and out-of-range chunk IDs refused");
    uint32_t index;
    lane_check(arcpack_chunk_parse(p, NAME ":2:1junk", strlen(NAME ":2:1"), &index) && index == 3,
               "chunk ID parsed by length, not NUL");
}

static void check_plain(void) {
    char buf[512];
    size_t n = arcpack_plain(buf, DOC0, TEXT_LENS[0]);
    lane_check(same(buf, n, DOC0_PLAIN, sizeof(DOC0_PLAIN) - 1), "plain text of the marked-up document");
    memcpy(buf, DOC0, TEXT_LENS[0]);
    n = arcpack_plain(buf, buf, TEXT_LENS[0]);
    lane_check(same(buf, n, DOC0_PLAIN, sizeof(DOC0_PLAIN) - 1), "plain text in place");
    static const struct {
        const char *in;
        const char *out;
    } cases[] = {
        {"plain text\twith tab\n", "plain text\twith tab\n"},
        {"\x01"
         "3Heading",
         "Heading"},
        {"a \x02"
         "12\x1F"
         "b\x03 c",
         "a b c"},
        {"\x04ref\x1F\n", "\n"},
        {"\x04ref-only\nnext", "\nnext"},
        {"cut \x02"
         "12",
         "cut "},
        {"stray \x1F and \x03 bytes", "stray  and  bytes"},
        {"\xC2\xB0"
         "C \xE2\x80\x94 ok",
         "\xC2\xB0"
         "C \xE2\x80\x94 ok"},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        n = arcpack_plain(buf, cases[i].in, strlen(cases[i].in));
        if (!same(buf, n, cases[i].out, strlen(cases[i].out))) {
            printf("plain case %zu wrong: '%.*s'\n", i, (int)n, buf);
            ok = false;
        }
    }
    lane_check(ok, "plain text: headings, links, images, cut and stray markup, UTF-8 kept");
    lane_check(arcpack_plain(buf, "", 0) == 0, "plain text of nothing");
}

/*
 * Documents the writer must refuse. Markup bytes are written as 3-digit octal escapes (\001 heading,
 * \002 link, \003 link end, \004 image, \037 separator) so each literal stays one string.
 */
/* Texts with markup or bytes pack-v1 forbids, grouped by the text each refusal message must contain. */
static const struct {
    const char *want;
    const char *texts[6]; /* NULL-terminated */
} BAD_TEXTS[] = {
    {"UTF-8", {"bad \303\050 utf-8"}},
    {"control character", {"crlf\r\n", "mid-line \0011heading"}},
    {"heading", {"\0017Too deep\n", "\0011\n"}},
    {"link", {"\00201\037x\003", "\0021\037\003", "\0021\037open", "\0021\037a\nb\003", "\002x\037a\003"}},
    {"image", {"\004no-separator\n", "\004\037caption\n"}},
};

/* A document with one chunk that must be refused, and the text its message must contain. */
typedef struct {
    const char *text; /* length from TEXT(): may hold NUL */
    size_t text_len;
    arcpack_chunk_spec chunk;
    const char *want;
} refusal;

#define TEXT(s) s, sizeof(s) - 1

static const refusal BAD_CHUNKS[] = {
    {TEXT("nul \0 byte"), {0, 1, NULL, 0, 0}, "control character 0x00"},
    {TEXT("hello"), {0, 6, NULL, 0, 0}, "outside the text"},
    {TEXT("hello"), {0, 0, NULL, 0, 0}, "outside the text"},
    {TEXT("\302\260C"), {0, 1, NULL, 0, 0}, "cut inside"},
    {TEXT("\0020\037ab\003"), {0, 4, NULL, 0, 0}, "cut inside"},
    {TEXT("\0011Head\n"), {1, 3, NULL, 0, 0}, "cut inside"},
    {TEXT("x"), {0, 1, "a\037\037b", 4, 0}, "section"},
    {TEXT("x"), {0, 1, "a\nb", 3, 0}, "section"},
};

/* refused_one: adding the document fails with a message containing `want`. */
static bool refused_one(arcpack_writer *w, const char *title, const char *text, size_t len,
                        const arcpack_chunk_spec *chunk, const char *want) {
    const char *why = "";
    bool added = arcpack_writer_add_doc(w, title, strlen(title), text, len, chunk, chunk ? 1 : 0, &why);
    if (added || strstr(why, want) == NULL) {
        printf("expected refusal '%s', got %s '%s'\n", want, added ? "success" : "refusal", why);
        return false;
    }
    return true;
}

/* refusals: every bad title, text and chunk is refused with its message, and the writer goes on. */
static bool refusals(arcpack_writer *w) {
    bool ok = refused_one(w, "", TEXT("x"), NULL, "title") & refused_one(w, "two\nlines", TEXT("x"), NULL, "title");
    for (size_t i = 0; i < sizeof(BAD_TEXTS) / sizeof(BAD_TEXTS[0]); i++) {
        for (const char *const *t = BAD_TEXTS[i].texts; *t != NULL; t++) {
            ok &= refused_one(w, "t", *t, strlen(*t), NULL, BAD_TEXTS[i].want);
        }
    }
    for (size_t i = 0; i < sizeof(BAD_CHUNKS) / sizeof(BAD_CHUNKS[0]); i++) {
        const refusal *r = &BAD_CHUNKS[i];
        ok &= refused_one(w, "t", r->text, r->text_len, &r->chunk, r->want);
    }
    return ok;
}

static bool exists(const char *dir, const char *rel) {
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, rel);
    return stat(path, &st) == 0;
}

static void check_writer_refusals(const char *scratch) {
    const char *why;
    lane_check(arcpack_writer_open(scratch, "Bad_Name", VERSION, &why) == NULL && strstr(why, "manifest id"),
               "pack name that is not a manifest id refused");
    lane_check(arcpack_writer_open(scratch, NAME, "1.0/x", &why) == NULL && strstr(why, "semver"),
               "bad version refused");

    arcpack_writer *w = arcpack_writer_open(scratch, NAME, VERSION, &why);
    if (w == NULL) {
        lane_check(false, why);
        return;
    }
    lane_check(refusals(w), "malformed titles, text, markup, chunk ranges and section paths refused");

    /* The writer goes on after refusals; a link past the last document fails finish(). */
    arcpack_chunk_spec all = {0, 5, NULL, 0, 0};
    static const char link_to_2[] = "\x02"
                                    "2\x1F"
                                    "two\x03";
    bool added = arcpack_writer_add_doc(w, "t", 1, TEXT("hello"), &all, 1, &why) &&
                 arcpack_writer_add_doc(w, "u", 1, TEXT(link_to_2), NULL, 0, &why);
    lane_check(added && arcpack_writer_doc_count(w) == 2 && arcpack_writer_chunk_count(w) == 1,
               "the writer goes on after refused documents");
    lane_check(!arcpack_writer_finish(w, &why) && strstr(why, "link names document 2"),
               "a link past the last document fails finish()");
    lane_check(!exists(scratch, "content/docs.idx") && !exists(scratch, "content/docs.idx.part") &&
                   !exists(scratch, "chunks/chunks.idx.part"),
               "a failed finish() leaves no pack files");

    w = arcpack_writer_open(scratch, NAME, VERSION, &why);
    lane_check(w != NULL && exists(scratch, "content/docs.zst.part"), "writer files start as .part");
    arcpack_writer_abort(w);
    lane_check(!exists(scratch, "content/docs.zst.part") && !exists(scratch, "content/docs.zst"), "abort removes them");
}

static int read3(const char *dir, const char *scratch) {
    const char *why;
    arcpack *p = arcpack_open(dir, &why);
    lane_check(p != NULL, p ? "open the three-document pack" : why);
    if (p == NULL) {
        return lane_check_done("arcpack");
    }
    lane_check(strcmp(arcpack_name(p), NAME) == 0 && strcmp(arcpack_version(p), VERSION) == 0,
               "name and version from the manifest");
    lane_check(arcpack_tag_count(p) == 3 && strcmp(arcpack_tag_name(p, 2), "outdoors") == 0 &&
                   arcpack_tag_name(p, 3) == NULL,
               "domain tag names in bit order");
    lane_check(arcpack_doc_count(p) == 3 && arcpack_chunk_count(p) == 4, "document and chunk counts");
    check_docs(p);
    check_chunks(p);
    arcpack_close(p);
    check_plain();
    check_writer_refusals(scratch);
    return lane_check_done("arcpack");
}

/* ---- Damaged packs ------------------------------------------------------------------------------ */

/* scan: read every document and chunk; buffers sized from the records, so damaged sizes show up as errors. */
static int scan(const char *dir) {
    const char *why;
    arcpack *p = arcpack_open(dir, &why);
    if (p == NULL) {
        printf("error: %s\n", why);
        return 1;
    }
    char id[ARCPACK_CHUNK_ID_MAX];
    bool ok = true;
    for (uint32_t d = 0; ok && d < arcpack_doc_count(p); d++) {
        arcpack_doc doc;
        size_t len;
        ok = arcpack_doc_get(p, d, &doc);
        char *buf = ok ? malloc(doc.text_size + 1u) : NULL;
        ok = buf != NULL && arcpack_doc_text(p, d, buf, doc.text_size, &len);
        free(buf);
    }
    for (uint32_t i = 0; ok && i < arcpack_chunk_count(p); i++) {
        arcpack_chunk c;
        size_t len;
        uint32_t back;
        ok = arcpack_chunk_get(p, i, &c);
        char *buf = ok ? malloc(c.doc_text_size + 1u) : NULL;
        ok = buf != NULL && arcpack_chunk_text(p, i, buf, c.doc_text_size, &len) &&
             arcpack_chunk_id(p, i, id, sizeof(id)) && arcpack_chunk_parse(p, id, strlen(id), &back);
        free(buf);
    }
    if (!ok) {
        printf("error: %s\n", arcpack_error(p));
    } else {
        printf("scan: %u documents, %u chunks, all readable\n", arcpack_doc_count(p), arcpack_chunk_count(p));
    }
    arcpack_close(p);
    return ok ? 0 : 1;
}

/* long_write: a one-document pack whose text (`mib` MiB) is larger than the 8 MiB window. */
static int long_write(const char *dir, uint32_t mib) {
    const char *why;
    size_t cap = (size_t)mib << 20, len = 0;
    char *text = malloc(cap + 64);
    arcpack_writer *w = text ? arcpack_writer_open(dir, NAME, VERSION, &why) : NULL;
    if (w == NULL) {
        fprintf(stderr, "long-write: %s\n", text ? why : "out of memory");
        free(text);
        return 1;
    }
    for (uint32_t line = 0; len < cap; line++) {
        len += (size_t)snprintf(text + len, 64,
                                "Line %u: water boils at 100 \xC2\xB0"
                                "C.\n",
                                line * 2654435761u);
    }
    arcpack_chunk_spec all = {0, (uint32_t)len, NULL, 0, 0};
    bool ok = arcpack_writer_add_doc(w, "Long", 4, text, len, &all, 1, &why);
    if (ok) {
        ok = arcpack_writer_finish(w, &why); /* frees the writer */
    } else {
        arcpack_writer_abort(w);
    }
    if (!ok) {
        fprintf(stderr, "long-write: %s\n", why);
    }
    free(text);
    return ok ? 0 : 1;
}

/* ---- The synthetic big pack --------------------------------------------------------------------- */

static int big_write(const char *dir, uint32_t n_chunks) {
    const char *why;
    arcpack_writer *w = arcpack_writer_open(dir, NAME, VERSION, &why);
    if (w == NULL) {
        fprintf(stderr, "big-write: %s\n", why);
        return 1;
    }
    static char text[BIG_DOC_CHUNKS * 128];
    static char sections[BIG_DOC_CHUNKS][32];
    arcpack_chunk_spec chunks[BIG_DOC_CHUNKS];
    bool ok = true;
    for (uint32_t first = 0, doc = 0; ok && first < n_chunks; first += BIG_DOC_CHUNKS, doc++) {
        uint32_t n = n_chunks - first < BIG_DOC_CHUNKS ? n_chunks - first : BIG_DOC_CHUNKS;
        size_t len = 0;
        for (uint32_t k = 0; k < n; k++) {
            int m = snprintf(text + len, sizeof(text) - len,
                             "Chunk %u of document %u: water boils at 100 \xC2\xB0"
                             "C at sea level.\n",
                             k, doc);
            int s = snprintf(sections[k], sizeof(sections[k]), "Part %u", k / 5);
            chunks[k] =
                (arcpack_chunk_spec){(uint32_t)len, (uint32_t)m, sections[k], (size_t)s, 1ull << ((first + k) % 64)};
            len += (size_t)m;
        }
        char title[32];
        int t = snprintf(title, sizeof(title), "Document %u", doc);
        ok = arcpack_writer_add_doc(w, title, (size_t)t, text, len, chunks, n, &why);
    }
    if (!ok) {
        fprintf(stderr, "big-write: %s\n", why);
        arcpack_writer_abort(w);
        return 1;
    }
    uint32_t docs = arcpack_writer_doc_count(w);
    if (!arcpack_writer_finish(w, &why)) {
        fprintf(stderr, "big-write: %s\n", why);
        return 1;
    }
    printf("big-write: %u documents, %u chunks\n", docs, n_chunks);
    return 0;
}

static int big_lookup(const char *dir) {
    const char *why;
    arcpack *p = arcpack_open(dir, &why);
    lane_check(p != NULL, p ? "open the big pack" : why);
    if (p == NULL) {
        return lane_check_done("arcpack-big");
    }
    static char buf[BIG_DOC_CHUNKS * 128];
    char id[ARCPACK_CHUNK_ID_MAX];
    char want[64];
    uint32_t n = arcpack_chunk_count(p);
    uint32_t x = 2463534242u;
    int good = 0;
    for (int i = 0; i < LOOKUPS && n > 0; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        uint32_t index = x % n;
        arcpack_chunk c;
        size_t len;
        uint32_t back;
        int m =
            snprintf(want, sizeof(want), "Chunk %u of document %u:", index % BIG_DOC_CHUNKS, index / BIG_DOC_CHUNKS);
        if (arcpack_chunk_get(p, index, &c) && c.document == index / BIG_DOC_CHUNKS && c.tags == 1ull << (index % 64) &&
            arcpack_chunk_text(p, index, buf, sizeof(buf), &len) && len > (size_t)m &&
            memcmp(buf, want, (size_t)m) == 0 && arcpack_chunk_id(p, index, id, sizeof(id)) &&
            arcpack_chunk_parse(p, id, strlen(id), &back) && back == index) {
            good++;
        } else {
            printf("lookup of chunk %u failed: %s\n", index, arcpack_error(p));
        }
    }
    printf("big-lookup: %d of %d random lookups in %u chunks right\n", good, LOOKUPS, n);
    lane_check(n > 0 && good == LOOKUPS, "random chunk lookups: record, text and ID");
    arcpack_close(p);
    return lane_check_done("arcpack-big");
}

/*
 * measured: run big_lookup in a forked child and print the child's peak RSS. The child starts
 * from this small process, so its ru_maxrss is the lookups' own; a process started by the Python
 * driver would carry the driver's RSS from before exec.
 */
static int measured(const char *dir) {
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        int status = big_lookup(dir);
        fflush(stdout);
        _exit(status);
    }
    int status;
    struct rusage ru;
    if (child < 0 || wait4(child, &status, 0, &ru) != child) {
        perror("big-lookup: fork/wait4");
        return 1;
    }
    printf("lookup peak RSS: %ld KiB\n", ru.ru_maxrss);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "write3") == 0) {
        return write3(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "read3") == 0) {
        return read3(argv[2], argv[3]);
    }
    if (argc == 3 && strcmp(argv[1], "scan") == 0) {
        return scan(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "long-write") == 0) {
        return long_write(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
    }
    if (argc == 4 && strcmp(argv[1], "big-write") == 0) {
        return big_write(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
    }
    if (argc == 3 && strcmp(argv[1], "big-lookup") == 0) {
        return measured(argv[2]);
    }
    fprintf(stderr, "usage: arcpack_test write3|read3|scan|long-write|big-write|big-lookup DIR [...]\n");
    return 2;
}
