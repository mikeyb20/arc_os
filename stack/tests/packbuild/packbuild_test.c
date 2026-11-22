/*
 * packbuild_test — lane.packbuild (M3-03): packbuild's build and chunker on generated article streams
 * (lane tests never read a real ZIM), read back with libarcpack.
 *
 *   packbuild_test DIR      run every check, writing streams and packs under DIR (emptied first,
 *                           removed when every check passes)
 *
 * Checks: links resolve to document IDs and links to unknown keys become their text; headings give
 * section paths and a short section joins the next; long sections are cut at line ends, then sentence
 * ends, then between words, with the set overlap; on 400 generated documents every chunk's word count
 * (counted again from arcpack_plain of its text) is within the limits, it starts and ends on an allowed
 * boundary, consecutive chunks overlap and leave no word out; every chunk carries every tag; chunk IDs
 * round-trip through arcpack_chunk_parse; two builds of one stream are byte-identical; a stream out of
 * order, with a repeated key, bad markup, a control character, an over-long line, no header or a cut
 * last line is refused and leaves nothing behind, and a non-empty output directory is refused untouched.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../lane_check.h"
#include "arcjson.h"
#include "arcpack.h"
#include "arcutil.h"
#include "packbuild.h"

#define NAME    "test-pack"
#define VERSION "1.0.0"

static const char *TAGS[] = {"medical", "first-aid"};
static char *root;

typedef struct {
    const char *key, *title, *text;
} doc;

/* path: root/name in a static buffer (one of two, alternating). */
static const char *path(const char *name) {
    static char buf[2][4096];
    static int which;
    which ^= 1;
    snprintf(buf[which], sizeof(buf[which]), "%s/%s", root, name);
    return buf[which];
}

/* write_stream: a header line and one line per document (n of them; texts may hold any bytes). */
static void write_stream(const char *file, const doc *docs, size_t n) {
    FILE *f = fopen(file, "wb");
    if (f == NULL) {
        perror(file);
        exit(1);
    }
    fputs("{\"stream\": 1, \"source\": \"packbuild_test\", \"zim\": \"none\", \"date\": \"2026-11-22\"}\n", f);
    arcjson_writer w;
    arcjson_writer_init(&w);
    for (size_t i = 0; i < n; i++) {
        arcjson_writer_reset(&w);
        arcjson_write_object_begin(&w);
        arcjson_write_key(&w, "key");
        arcjson_write_string(&w, docs[i].key);
        arcjson_write_key(&w, "title");
        arcjson_write_string(&w, docs[i].title);
        arcjson_write_key(&w, "text");
        arcjson_write_string(&w, docs[i].text);
        arcjson_write_object_end(&w);
        size_t len;
        const char *s = arcjson_writer_result(&w, &len);
        fwrite(s, 1, len, f);
        fputc('\n', f);
    }
    arcjson_writer_free(&w);
    fclose(f);
}

static packbuild_options options(void) {
    return (packbuild_options){NAME, VERSION, TAGS, 2, packbuild_chunking_default};
}

/* build: packbuild_build of stream file into dir, then manifest.json so arcpack_open can read it. */
static bool run_build(const char *stream, const char *dir, packbuild_stats *st, const char **why) {
    packbuild_options opt = options();
    if (!packbuild_build(stream, dir, &opt, st, why)) {
        return false;
    }
    char m[512];
    int n = snprintf(m, sizeof(m),
                     "{\"schema_version\": 1, \"name\": \"%s\", \"version\": \"%s\", \"domain_tags\": [\"%s\", "
                     "\"%s\"], \"chunk_count\": %u, \"document_count\": %u}\n",
                     NAME, VERSION, TAGS[0], TAGS[1], st->chunks, st->documents);
    char mp[4096];
    snprintf(mp, sizeof(mp), "%s/manifest.json", dir);
    return arcutil_write_durable(mp, m, (size_t)n);
}

static arcpack *open_pack(const char *dir) {
    const char *why;
    arcpack *p = arcpack_open(dir, &why);
    if (p == NULL) {
        printf("arcpack_open %s: %s\n", dir, why);
    }
    return p;
}

/* doc_text: document d's text in a malloc'd NUL-terminated buffer. */
static char *doc_text(arcpack *p, uint32_t d, size_t *len) {
    arcpack_doc info;
    if (!arcpack_doc_get(p, d, &info)) {
        return NULL;
    }
    char *buf = malloc(info.text_size + 1);
    if (buf == NULL || !arcpack_doc_text(p, d, buf, info.text_size + 1, len)) {
        free(buf);
        return NULL;
    }
    buf[*len] = '\0';
    return buf;
}

/* plain_words: words of the plain text of s[0..len) (the independent count: arcpack_plain + arcpack_next_word). */
static uint32_t plain_words(const char *s, size_t len) {
    char *tmp = malloc(len + 1);
    size_t n = arcpack_plain(tmp, s, len);
    size_t pos = 0, start;
    uint32_t words = 0;
    while (arcpack_next_word(tmp, n, &pos, &start)) {
        words++;
    }
    free(tmp);
    return words;
}

static bool is_word_byte(const char *s, size_t len, size_t i) {
    uint32_t cp;
    size_t n = arcjson_utf8_decode(s + i, len - i, &cp);
    return n != 0 && arcpack_word_char(cp);
}

/* prev_char: the start of the character before byte i (i > 0). */
static size_t prev_char(const char *s, size_t i) {
    size_t j = i - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) {
        j--;
    }
    return j;
}

/* start_ok: a chunk may start at i: the text start, a line start, a link's start byte, or a word start. */
static bool start_ok(const char *s, size_t len, size_t i) {
    return i == 0 || s[i - 1] == '\n' || s[i] == ARCPACK_MARK_LINK ||
           (is_word_byte(s, len, i) && !is_word_byte(s, len, prev_char(s, i)));
}

/* end_ok: a chunk may end at i: the text end, a line end, a link's end, a sentence end or a word end. */
static bool end_ok(const char *s, size_t len, size_t i) {
    if (i == len || s[i - 1] == '\n' || s[i] == '\n' || s[i - 1] == ARCPACK_MARK_LINK_END) {
        return true;
    }
    if (strchr(".!?\")]'", s[i - 1]) != NULL) {
        return s[i] == ' ' || s[i] == '\t';
    }
    return is_word_byte(s, len, prev_char(s, i)) && !is_word_byte(s, len, i);
}

/* ---- Links, headings, short sections ------------------------------------------------------------ */

static void test_links_and_sections(void) {
    static const doc docs[] = {
        {"Alpha", "Alpha", "Go to \002Beta\037Bee\003 and \002Nowhere\037the void\003 and \002Alpha\037here\003."},
        {"Beta", "Beta",
         "Lead text with enough words to stand alone as its own section of twenty words or more, one two three.\n"
         "\0012Signs\nFirst signs are shivering and confusion, then slurred speech, clumsy hands, drowsiness and a "
         "slow weak pulse in most people.\n"
         "\0013Early\nEarly on the person shivers hard and feels cold, with numb fingers and toes, pale skin, fast "
         "breathing and some confusion.\n"
         "\0012See also\nFrostbite.\n"
         "\0012Treatment\nMove the person somewhere warm, take off wet clothes, wrap them in blankets and give warm "
         "sweet drinks if they are awake and can swallow."},
        {"Gamma", "Gamma", ""},
    };
    write_stream(path("links.jsonl"), docs, 3);
    packbuild_stats st;
    const char *why;
    lane_check(run_build(path("links.jsonl"), path("links"), &st, &why), "build a three-document stream");
    lane_check(st.documents == 3 && st.links == 2 && st.links_dropped == 1 && st.docs_no_chunks == 1,
               "two links resolved, one to an unknown key dropped, the empty document has no chunk");
    arcpack *p = open_pack(path("links"));
    if (p == NULL) {
        lane_check(false, "arcpack_open reads the pack");
        return;
    }
    size_t len;
    char *t = doc_text(p, 0, &len);
    const char want[] = "Go to \0021\037Bee\003 and the void and \0020\037here\003.";
    lane_check(t && len == sizeof(want) - 1 && memcmp(t, want, len) == 0, "links name document IDs; unknown is text");
    free(t);
    arcpack_doc d;
    lane_check(arcpack_doc_get(p, 1, &d) && d.chunk_count == 4,
               "Beta: four chunks (lead, Signs, Early, See also+Treatment)");
    static const char *paths[] = {"", "Signs", "Signs\037Early", "See also"};
    bool ok = true;
    for (uint32_t i = 0; i < 4 && ok; i++) {
        arcpack_chunk c;
        ok = arcpack_chunk_get(p, d.first_chunk + i, &c) && c.section_length == strlen(paths[i]) &&
             memcmp(c.section, paths[i], c.section_length) == 0;
    }
    lane_check(ok, "section paths: headings above each chunk; the short See also section joins Treatment");
    arcpack_chunk c;
    t = doc_text(p, 1, &len);
    ok = t && arcpack_chunk_get(p, d.first_chunk + 3, &c) && c.offset + c.length == len &&
         strstr(t + c.offset, "\0012Treatment") != NULL;
    lane_check(ok, "the joined chunk runs to the end of Treatment");
    ok = t && arcpack_chunk_get(p, d.first_chunk + 1, &c) && memcmp(t + c.offset, "\0012Signs\n", 8) == 0;
    lane_check(ok, "a section's chunk starts with its heading line");
    free(t);
    arcpack_close(p);
}

/* ---- Cut rules on one long section ---------------------------------------------------------------- */

/* words_text: n words "wI", with `sep` after every k-th word (else a space). */
static char *words_text(unsigned n, unsigned k, const char *sep) {
    char *s = malloc((size_t)n * 16 + 1);
    size_t o = 0;
    for (unsigned i = 0; i < n; i++) {
        o += (size_t)sprintf(s + o, "w%u", i);
        if (i + 1 < n) {
            o += (size_t)sprintf(s + o, "%s", (k && (i + 1) % k == 0) ? sep : " ");
        }
    }
    s[o] = '\0';
    return s;
}

/* chunk_words: the chunker's chunks of `text` as word counts and first words (index of "wI"). */
static size_t chunk_words(const char *text, uint32_t *words, unsigned *first, size_t max) {
    packbuild_chunks c = {0};
    packbuild_chunk_text(text, strlen(text), &packbuild_chunking_default, &c);
    size_t n = c.n < max ? c.n : max;
    for (size_t i = 0; i < n; i++) {
        words[i] = c.chunk[i].words;
        first[i] = (unsigned)strtoul(text + c.chunk[i].offset + 1, NULL, 10);
    }
    packbuild_chunks_free(&c);
    return n;
}

/* long_construct_ok: the chunks of t move forward, none is empty, and only one holding the 500-word image line
 * alone is over the maximum. */
static bool long_construct_ok(const char *t, const packbuild_chunks *c, uint32_t first_after) {
    bool ok = c->n >= 3;
    for (size_t i = 0; ok && i < c->n; i++) {
        const packbuild_chunk *k = &c->chunk[i];
        ok = k->length > 0 && k->words > 0 &&
             (i == 0 || (k->offset >= c->chunk[i - 1].offset &&
                         k->offset + k->length > c->chunk[i - 1].offset + c->chunk[i - 1].length));
        if (ok && k->words > PACKBUILD_MAX_WORDS) {
            ok = k->words == 500 && t[k->offset] == ARCPACK_MARK_IMAGE && t[k->offset + k->length] == '\n';
        }
    }
    return ok && c->chunk[c->n - 1].words == first_after;
}

/* test_long_construct: an image line longer than the maximum (a 500-word caption, seen in WikiMed maxi). */
static void test_long_construct(void) {
    char *a = words_text(300, 0, "");
    char *cap = words_text(500, 0, "");
    char *b = words_text(50, 0, "");
    size_t need = 2 * strlen(a) + strlen(cap) + strlen(b) + 64; /* room for either text */
    char *t = malloc(need);
    packbuild_chunks c = {0};
    /* It ends its section: the window before it stops at its start, and no empty chunk follows it (the last chunk
     * is the next section: "Next" and 50 words). */
    snprintf(t, need, "%s\n\004img.png\037%s\n\0012Next\n%s", a, cap, b);
    lane_check(packbuild_chunk_text(t, strlen(t), &packbuild_chunking_default, &c) && long_construct_ok(t, &c, 51),
               "an image line over the maximum at a section's end: one chunk of its own, none empty or repeated");
    /* Mid-section, followed by more words. */
    snprintf(t, need, "%s\n\004img.png\037%s\n%s", a, cap, a);
    lane_check(packbuild_chunk_text(t, strlen(t), &packbuild_chunking_default, &c) && long_construct_ok(t, &c, 300),
               "an image line over the maximum mid-section: one chunk of its own, then the words after it");
    packbuild_chunks_free(&c);
    free(a);
    free(cap);
    free(b);
    free(t);
}

static void test_cut_rules(void) {
    uint32_t w[16];
    unsigned f[16];
    char *t = words_text(1000, 0, "");
    size_t n = chunk_words(t, w, f, 16);
    lane_check(n == 4 && w[0] == 280 && w[1] == 280 && w[2] == 280 && w[3] == 280 && f[1] == 240 && f[2] == 480 &&
                   f[3] == 720,
               "no line or sentence ends: cut after 280 words, next starts 40 words back, last takes the rest");
    free(t);
    t = words_text(1000, 50, "\n");
    n = chunk_words(t, w, f, 16);
    lane_check(n >= 2 && w[0] == 250 && f[1] == 210, "a line end in the last quarter before the target wins (250)");
    free(t);
    t = words_text(1000, 30, ". ");
    n = chunk_words(t, w, f, 16);
    lane_check(n >= 2 && w[0] == 270 && f[1] == 230, "else the sentence end nearest the target (270 of 270/300)");
    free(t);
    t = words_text(1000, 70, ". ");
    n = chunk_words(t, w, f, 16);
    lane_check(n >= 2 && w[0] == 280, "sentence ends at 210 and 280: the one nearest the target (280)");
    free(t);
    t = words_text(1000, 290, ". ");
    n = chunk_words(t, w, f, 16);
    lane_check(n >= 2 && w[0] == 290, "a sentence end past the target, up to the maximum, is used (290)");
    free(t);
    t = words_text(350, 0, "");
    n = chunk_words(t, w, f, 16);
    lane_check(n == 1 && w[0] == 350, "a section of at most 360 words is one chunk");
    free(t);
    /* A link around words 270-290: the cut never falls inside it. */
    char *a = words_text(270, 0, "");
    char *b = words_text(300, 0, "");
    size_t need = strlen(a) + strlen(b) + 300;
    t = malloc(need);
    snprintf(t, need, "%s \0027\037l0 l1 l2 l3 l4 l5 l6 l7 l8 l9 l10 l11 l12 l13 l14 l15 l16 l17 l18 l19\003 %s", a, b);
    packbuild_chunks c = {0};
    packbuild_chunk_text(t, strlen(t), &packbuild_chunking_default, &c);
    lane_check(c.n >= 2 && t[c.chunk[0].offset + c.chunk[0].length - 1] == ARCPACK_MARK_LINK_END &&
                   c.chunk[0].words == 290,
               "a cut that would fall inside a link moves to the link's end");
    packbuild_chunks_free(&c);
    free(a);
    free(b);
    free(t);
    test_long_construct();
}

/* ---- Generated documents: invariants ---------------------------------------------------------------- */

static uint64_t rng = 0x9E3779B97F4A7C15u;

static unsigned rnd(unsigned n) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng % n);
}

/* Words, with an accent, a Greek word, a number and a hyphen (two words). */
static const char VOCAB_TEXT[] =
    "fever cold water \xc3\xa9t\xc3\xa9 \xce\xb4\xce\xbf\xcf\x83\xce\xb7 pulse 35 splint wound "
    "burn rest heat x-ray CPR";
static const char *VOCAB[14];

/* vocab_init: split VOCAB_TEXT into VOCAB (a copy, so the words can be NUL-terminated). */
static void vocab_init(void) {
    static char words[sizeof(VOCAB_TEXT)];
    memcpy(words, VOCAB_TEXT, sizeof(words));
    size_t n = 0;
    for (char *w = strtok(words, " "); w != NULL && n < 14; w = strtok(NULL, " ")) {
        VOCAB[n++] = w;
    }
}

/* gen_text: a random document: lines of words with punctuation, headings, links, images, table rows. */
static char *gen_text(unsigned n_docs, size_t *len) {
    size_t cap = 1 << 16, o = 0;
    char *s = malloc(cap);
    unsigned lines = 1 + rnd(60);
    for (unsigned l = 0; l < lines; l++) {
        if (o + 8192 > cap) {
            cap *= 2;
            s = realloc(s, cap);
        }
        unsigned kind = rnd(20);
        if (kind == 0) {
            o += (size_t)sprintf(s + o, "\001%u%s %s", 1 + rnd(6), VOCAB[rnd(14)], VOCAB[rnd(14)]);
        } else if (kind == 1) {
            o += (size_t)sprintf(s + o, "\004img/%u.webp\037%s %s", rnd(100), rnd(3) ? VOCAB[rnd(14)] : "",
                                 VOCAB[rnd(14)]);
        } else {
            unsigned words = rnd(8) == 0 ? 400 + rnd(400) : rnd(120);
            for (unsigned w = 0; w < words; w++) {
                unsigned r = rnd(40);
                if (r == 0) {
                    o += (size_t)sprintf(s + o, "\002%u\037%s %s\003", rnd(n_docs), VOCAB[rnd(14)], VOCAB[rnd(14)]);
                } else {
                    o += (size_t)sprintf(s + o, "%s", VOCAB[rnd(14)]);
                }
                static const char *const SEP[] = {" ", " ", " ", " ", ", ", ". ", "! ", "? ", ".) ", "\t", " (", "; "};
                if (w + 1 < words) {
                    o += (size_t)sprintf(s + o, "%s", SEP[rnd(12)]);
                }
                if (o + 512 > cap) {
                    cap *= 2;
                    s = realloc(s, cap);
                }
            }
            if (words && rnd(2)) {
                s[o++] = '.';
            }
        }
        if (l + 1 < lines) {
            s[o++] = '\n';
        }
    }
    s[o] = '\0';
    *len = o;
    return s;
}

/* gen_docs: n random documents with keys k00000.., their links naming those keys. */
static doc *gen_docs(unsigned n, char (*keys)[16]) {
    doc *docs = calloc(n, sizeof(*docs));
    for (unsigned i = 0; i < n; i++) {
        snprintf(keys[i], 16, "k%05u", i);
        size_t len;
        docs[i] = (doc){keys[i], keys[i], NULL};
        char *t = gen_text(n, &len);
        /* links in the generator name document numbers; turn them into keys */
        size_t cap = len * 2 + 64, o = 0;
        char *k = malloc(cap);
        for (size_t j = 0; j < len; j++) {
            if (t[j] == ARCPACK_MARK_LINK) {
                unsigned id = (unsigned)strtoul(t + j + 1, NULL, 10);
                o += (size_t)sprintf(k + o, "\002k%05u", id);
                while (t[j + 1] != ARCPACK_MARK_SEP) {
                    j++;
                }
                continue;
            }
            if (o + 16 > cap) {
                cap *= 2;
                k = realloc(k, cap);
            }
            k[o++] = t[j];
        }
        k[o] = '\0';
        free(t);
        docs[i].text = k;
    }
    return docs;
}

/* check_rebuild: a second build of gen.jsonl is byte-identical to the first. */
static void check_rebuild(void) {
    packbuild_stats st;
    const char *why = "";
    lane_check(run_build(path("gen.jsonl"), path("gen2"), &st, &why), "build the same stream again");
    static const char *const files[] = {"content/docs.idx", "content/docs.zst", "content/strings", "chunks/chunks.idx"};
    bool same = true;
    for (int i = 0; i < 4; i++) {
        char a[4096], b[4096];
        snprintf(a, sizeof(a), "%s/gen/%s", root, files[i]);
        snprintf(b, sizeof(b), "%s/gen2/%s", root, files[i]);
        size_t la, lb;
        const char *w1, *w2;
        char *x = arcjson_read_file(a, 1u << 30, &la, &w1);
        char *y = arcjson_read_file(b, 1u << 30, &lb, &w2);
        same &= x && y && la == lb && memcmp(x, y, la) == 0;
        free(x);
        free(y);
    }
    lane_check(same, "two builds of one stream are byte-identical");
}

static void test_generated(void) {
    enum { N = 400 };
    char(*keys)[16] = calloc(N, 16);
    doc *docs = gen_docs(N, keys);
    write_stream(path("gen.jsonl"), docs, N);
    packbuild_stats st;
    const char *why = "";
    bool built = run_build(path("gen.jsonl"), path("gen"), &st, &why);
    if (!built) {
        printf("build: %s\n", why);
    }
    lane_check(built && st.documents == N, "build 400 generated documents");
    arcpack *p = built ? open_pack(path("gen")) : NULL;
    bool words_ok = p != NULL, starts_ok = true, ends_ok = true, cover_ok = true, overlap_ok = true, tags_ok = true,
         ids_ok = true, count_ok = true;
    for (uint32_t d = 0; p && d < arcpack_doc_count(p); d++) {
        arcpack_doc info;
        size_t len;
        char *t = doc_text(p, d, &len);
        if (t == NULL || !arcpack_doc_get(p, d, &info)) {
            words_ok = false;
            break;
        }
        uint32_t total = 0;
        uint32_t covered = 0; /* plain words up to the end of the chunk before */
        uint32_t prev_end = 0, prev_words_end = 0;
        for (uint32_t i = 0; i < info.chunk_count; i++) {
            arcpack_chunk c;
            uint32_t idx = info.first_chunk + i;
            if (!arcpack_chunk_get(p, idx, &c)) {
                words_ok = false;
                break;
            }
            uint32_t w = plain_words(t + c.offset, c.length);
            uint32_t before = plain_words(t, c.offset);
            words_ok &= w >= 1 && w <= PACKBUILD_MAX_WORDS;
            starts_ok &= start_ok(t, len, c.offset);
            ends_ok &= end_ok(t, len, c.offset + c.length);
            tags_ok &= c.tags == 3;
            char id[ARCPACK_CHUNK_ID_MAX];
            uint32_t back;
            ids_ok &= arcpack_chunk_id(p, idx, id, sizeof(id)) && arcpack_chunk_parse(p, id, strlen(id), &back) &&
                      back == idx;
            /* Overlapping chunks share 1..40 words, plus up to 3 at each end when a cut or a start moves out of
             * a link, heading or image line (the generator's hold at most 4 words: "x-ray" is two). */
            if (i > 0 && c.offset < prev_end) {
                bool o = prev_words_end > before && prev_words_end - before <= PACKBUILD_OVERLAP + 6;
                if (!o) {
                    printf("  document %u, chunk %u: overlaps the one before by %d words\n", d, i,
                           (int)prev_words_end - (int)before);
                }
                overlap_ok &= o;
            }
            cover_ok &= before <= covered; /* no word between this chunk and the one before */
            covered = before + w;
            prev_end = c.offset + c.length;
            prev_words_end = before + w;
            total = covered;
        }
        count_ok &= total == plain_words(t, len);
        free(t);
    }
    lane_check(words_ok, "every chunk holds 1..360 words, counted again from arcpack_plain of its text");
    lane_check(starts_ok, "every chunk starts at a line, link or word start");
    lane_check(ends_ok, "every chunk ends at a line, link, sentence or word end");
    lane_check(overlap_ok,
               "consecutive chunks of a section overlap by the set 40 words (a link or line counted whole)");
    lane_check(cover_ok && count_ok, "the chunks of each document leave no word out");
    lane_check(tags_ok, "every chunk carries every tag (bits 0 and 1)");
    lane_check(ids_ok, "every chunk ID round-trips through arcpack_chunk_parse");
    arcpack_close(p);

    check_rebuild();
    for (unsigned i = 0; i < N; i++) {
        free((char *)docs[i].text);
    }
    free(docs);
    free(keys);
}

/* ---- Refusals ----------------------------------------------------------------------------------- */

static bool gone(const char *dir) {
    struct stat sb;
    return stat(dir, &sb) != 0 && errno == ENOENT;
}

static bool empty_dir(const char *dir) {
    return rmdir(dir) == 0 && mkdir(dir, 0755) == 0;
}

/* refused: building `stream` fails with a message containing `want` and leaves no pack directory. */
static void refused(const char *stream, const char *want, const char *what) {
    packbuild_stats st;
    const char *why = "";
    packbuild_options opt = options();
    bool ok = !packbuild_build(stream, path("bad"), &opt, &st, &why) && strstr(why, want) != NULL && gone(path("bad"));
    if (!ok) {
        printf("  why: %s\n", why);
    }
    lane_check(ok, what);
}

static void write_raw(const char *file, const char *data) {
    FILE *f = fopen(file, "wb");
    fputs(data, f);
    fclose(f);
}

static void test_refusals(void) {
    static const doc order[] = {{"b", "B", "x"}, {"a", "A", "y"}};
    write_stream(path("order.jsonl"), order, 2);
    refused(path("order.jsonl"), "not after the previous", "keys out of order are refused");
    static const doc twice[] = {{"a", "A", "x"}, {"a", "A", "y"}};
    write_stream(path("twice.jsonl"), twice, 2);
    refused(path("twice.jsonl"), "not after the previous", "a repeated key is refused");
    static const doc link[] = {{"a", "A", "see \002b\037bee and more"}, {"b", "B", "x"}};
    write_stream(path("link.jsonl"), link, 2);
    refused(path("link.jsonl"), "malformed link", "a link that doesn't close is refused");
    static const doc ctrl[] = {{"a", "A", "bad \005 byte"}};
    write_stream(path("ctrl.jsonl"), ctrl, 1);
    refused(path("ctrl.jsonl"), "control character", "a control character in the text is refused (by the writer)");
    static const doc heading[] = {{"a", "A", "\0019Nine"}};
    write_stream(path("heading.jsonl"), heading, 1);
    refused(path("heading.jsonl"), "malformed", "a heading of level 9 is refused");
    static const doc notitle[] = {{"a", "", "text"}};
    write_stream(path("notitle.jsonl"), notitle, 1);
    refused(path("notitle.jsonl"), "title", "an empty title is refused");
    write_raw(path("nohead.jsonl"), "{\"key\": \"a\", \"title\": \"A\", \"text\": \"x\"}\n");
    refused(path("nohead.jsonl"), "header", "a stream without its header line is refused");
    write_raw(path("cut.jsonl"), "{\"stream\": 1}\n{\"key\": \"a\", \"title\": \"A\", \"text\": \"x\"}");
    refused(path("cut.jsonl"), "no newline", "a last line without its newline (file cut short) is refused");
    write_raw(path("json.jsonl"), "{\"stream\": 1}\n{\"key\": \"a\", \"title\": \"A\", \"text\": }\n");
    refused(path("json.jsonl"), "line 2", "a line that isn't JSON is refused, naming the line");
    write_raw(path("nokey.jsonl"), "{\"stream\": 1}\n{\"title\": \"A\", \"text\": \"x\"}\n");
    refused(path("nokey.jsonl"), "\"key\"", "a document without a key is refused");
    refused(path("missing.jsonl"), "No such file", "a missing stream is refused");

    /* A line one byte over the limit (the limit itself is fine: its JSON is still parsed). */
    FILE *f = fopen(path("long.jsonl"), "wb");
    fputs("{\"stream\": 1}\n{\"key\": \"a\", \"title\": \"A\", \"text\": \"", f);
    size_t head = strlen("{\"key\": \"a\", \"title\": \"A\", \"text\": \"");
    char *fill = malloc(1 << 20);
    memset(fill, 'x', 1 << 20);
    size_t left = PACKBUILD_LINE_MAX + 1 - head - 2;
    while (left > 0) {
        size_t n = left < (1u << 20) ? left : (1u << 20);
        fwrite(fill, 1, n, f);
        left -= n;
    }
    fputs("\"}\n", f);
    fclose(f);
    free(fill);
    refused(path("long.jsonl"), "longer than", "a line over 64 MiB is refused");
    unlink(path("long.jsonl"));

    /* An empty output directory given to packbuild stays (empty) after a failure. */
    mkdir(path("bad"), 0755);
    packbuild_stats st;
    const char *why;
    packbuild_options opt = options();
    lane_check(!packbuild_build(path("order.jsonl"), path("bad"), &opt, &st, &why) && empty_dir(path("bad")),
               "a failed build into an empty directory leaves it empty");
    rmdir(path("bad"));
    /* A directory that holds something is refused and left as it was. */
    mkdir(path("full"), 0755);
    write_raw(path("full/keep"), "keep");
    static const doc ok1[] = {{"a", "A", "fine"}};
    write_stream(path("ok.jsonl"), ok1, 1);
    struct stat sb;
    lane_check(!packbuild_build(path("ok.jsonl"), path("full"), &opt, &st, &why) && strstr(why, "not an empty") &&
                   stat(path("full/keep"), &sb) == 0,
               "an output directory that isn't empty is refused and left alone");
    opt.n_tags = 2;
    static const char *dup[] = {"medical", "medical"};
    opt.tags = dup;
    lane_check(!packbuild_build(path("ok.jsonl"), path("dup"), &opt, &st, &why) && strstr(why, "twice") &&
                   gone(path("dup")),
               "a tag given twice is refused");
    static const char *badtag[] = {"Medical"};
    opt.tags = badtag;
    opt.n_tags = 1;
    lane_check(!packbuild_build(path("ok.jsonl"), path("dup"), &opt, &st, &why) && strstr(why, "manifest id"),
               "a tag that isn't a manifest id is refused");
    opt = options();
    opt.name = "Bad_Name";
    lane_check(!packbuild_build(path("ok.jsonl"), path("dup"), &opt, &st, &why) && gone(path("dup")),
               "a pack name that isn't a manifest id is refused, nothing left");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: packbuild_test DIR\n", stderr);
        return 2;
    }
    root = argv[1];
    vocab_init();
    if (!arcutil_remove_tree(root) || !arcutil_make_dirs(root)) {
        perror(root);
        return 1;
    }
    test_links_and_sections();
    test_cut_rules();
    test_generated();
    test_refusals();
    int rc = lane_check_done("packbuild");
    if (rc == 0) {
        arcutil_remove_tree(root);
    }
    return rc;
}
