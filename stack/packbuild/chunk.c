/*
 * chunk.c — cutting one document's marked-up text into chunks (packbuild.h has the rules).
 *
 * One pass over the text collects the words (with the markup construct each lies in), the line
 * ends, the sentence ends and the sections; then each run of sections (a short one joined to the
 * next) is cut by word counts. Word positions are counted the way arcpack_plain() would show the
 * text: heading markers, link targets and image references are not words.
 */
#include <stdlib.h>
#include <string.h>

#include "arcpack.h"
#include "packbuild.h"

const packbuild_chunking packbuild_chunking_default = {PACKBUILD_TARGET_WORDS, PACKBUILD_MAX_WORDS, PACKBUILD_OVERLAP,
                                                       PACKBUILD_MIN_SECTION};

/* A word and the markup construct it lies in (a link, a heading line, an image line, or just itself). */
typedef struct {
    uint32_t cstart, cend;  /* bytes where a chunk may start before it / end after it */
    uint32_t cfirst, clast; /* first and last word of the same construct */
} word;

/* A cut candidate: a byte position and the number of words before it. */
typedef struct {
    uint32_t pos;
    uint32_t words;
} mark;

/* A section: from a heading line (or the start of the text) to the next heading. */
typedef struct {
    uint32_t start;     /* byte */
    uint32_t first;     /* first word */
    uint32_t path_off;  /* into packbuild_chunks.paths */
    uint32_t path_len;
} section;

struct packbuild_scratch {
    word *w;
    size_t nw, cw;
    mark *line, *sent;
    size_t nl, cl, ns, cs;
    section *sec;
    size_t nsec, csec;
};

/*
 * grow_array: make room for one more item in the array whose pointer is at `arrp` (n used, *cap allocated,
 * item size sz). The pointer is copied in and out with memcpy, so any element type works without
 * type-punning through void **.
 */
static bool grow_array(void *arrp, size_t n, size_t *cap, size_t sz) {
    if (n < *cap) {
        return true;
    }
    void *arr;
    memcpy(&arr, arrp, sizeof(arr));
    size_t c = *cap ? 2 * *cap : 256;
    void *p = realloc(arr, c * sz);
    if (p == NULL) {
        return false;
    }
    memcpy(arrp, &p, sizeof(p));
    *cap = c;
    return true;
}

static bool add_mark(mark **arr, size_t *n, size_t *cap, uint32_t pos, uint32_t words) {
    if (!grow_array(arr, *n, cap, sizeof(**arr))) {
        return false;
    }
    (*arr)[(*n)++] = (mark){pos, words};
    return true;
}

/* add_words: the words of plain text s[a, b), all inside construct [cs, ce) (cs == ce: each word is its own). */
static bool add_words(struct packbuild_scratch *x, const char *s, size_t a, size_t b, uint32_t cs, uint32_t ce) {
    size_t pos = a;
    size_t start;
    uint32_t first = (uint32_t)x->nw;
    while (arcpack_next_word(s, b, &pos, &start)) {
        if (!grow_array(&x->w, x->nw, &x->cw, sizeof(*x->w))) {
            return false;
        }
        bool own = cs == ce;
        x->w[x->nw] = (word){own ? (uint32_t)start : cs, own ? (uint32_t)pos : ce, own ? (uint32_t)x->nw : first, 0};
        x->nw++;
    }
    for (size_t i = first; i < x->nw; i++) {
        x->w[i].clast = cs == ce ? (uint32_t)i : (uint32_t)x->nw - 1;
    }
    return true;
}

/* is_closer: a byte that may follow a sentence's punctuation before the space: quotes and brackets. */
static bool is_closer(const char *s, size_t i, size_t b, size_t *len) {
    if (s[i] == '"' || s[i] == '\'' || s[i] == ')' || s[i] == ']') {
        *len = 1;
        return true;
    }
    /* ’ U+2019 and ” U+201D */
    if (i + 2 < b && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
        ((unsigned char)s[i + 2] == 0x99 || (unsigned char)s[i + 2] == 0x9D)) {
        *len = 3;
        return true;
    }
    return false;
}

/* scan_plain: plain text s[a, b) of an ordinary line: its words, and its sentence ends. */
static bool scan_plain(struct packbuild_scratch *x, const char *s, size_t a, size_t b) {
    size_t k = x->nw; /* words of this run ending at or before the sentence end */
    if (!add_words(x, s, a, b, 0, 0)) {
        return false;
    }
    for (size_t i = a; i < b; i++) {
        if (s[i] != '.' && s[i] != '!' && s[i] != '?') {
            continue;
        }
        size_t j = i + 1;
        size_t n;
        while (j < b && is_closer(s, j, b, &n)) {
            j += n;
        }
        if (j == b || s[j] == ' ' || s[j] == '\t') {
            while (k < x->nw && x->w[k].cend <= j) {
                k++;
            }
            if (!add_mark(&x->sent, &x->ns, &x->cs, (uint32_t)j, (uint32_t)k)) {
                return false;
            }
        }
    }
    return true;
}

/* ordinary_line: s[a, b), a line that is not a heading or an image: plain runs between links. */
static bool ordinary_line(struct packbuild_scratch *x, const char *s, size_t a, size_t b) {
    size_t run = a;
    for (size_t i = a; i < b; i++) {
        if (s[i] != ARCPACK_MARK_LINK) {
            continue;
        }
        const char *sep = memchr(s + i, ARCPACK_MARK_SEP, b - i);
        const char *close = sep ? memchr(sep, ARCPACK_MARK_LINK_END, (size_t)(s + b - sep)) : NULL;
        if (close == NULL) {
            continue; /* malformed: the writer refuses it */
        }
        size_t end = (size_t)(close - s) + 1;
        if (!scan_plain(x, s, run, i) || !add_words(x, s, (size_t)(sep - s) + 1, end - 1, (uint32_t)i, (uint32_t)end)) {
            return false;
        }
        run = end;
        i = end - 1;
    }
    return scan_plain(x, s, run, b);
}

/* new_section: a section starting at byte `start` whose path is the headings in levels[1..6]. */
static bool new_section(struct packbuild_scratch *x, packbuild_chunks *out, const char *s, uint32_t start,
                        uint32_t lv[7][2]) {
    if (x->nsec > 0 && x->sec[x->nsec - 1].start == start) {
        x->nsec--; /* a heading at the very start replaces the empty lead section */
    }
    if (!grow_array(&x->sec, x->nsec, &x->csec, sizeof(*x->sec))) {
        return false;
    }
    size_t off = out->paths_len;
    for (int l = 1; l <= 6; l++) {
        uint32_t n = lv[l][1] - lv[l][0];
        if (n == 0) {
            continue;
        }
        size_t need = out->paths_len + n + 1;
        if (need > out->paths_cap) {
            size_t c = out->paths_cap ? out->paths_cap : 1024;
            while (c < need) {
                c *= 2;
            }
            char *p = realloc(out->paths, c);
            if (p == NULL) {
                return false;
            }
            out->paths = p;
            out->paths_cap = c;
        }
        if (out->paths_len > off) {
            out->paths[out->paths_len++] = ARCPACK_MARK_SEP;
        }
        memcpy(out->paths + out->paths_len, s + lv[l][0], n);
        out->paths_len += n;
    }
    x->sec[x->nsec++] = (section){start, (uint32_t)x->nw, (uint32_t)off, (uint32_t)(out->paths_len - off)};
    return true;
}

/* scan_text: fill the scratch arrays from the text. */
static bool scan_text(struct packbuild_scratch *x, packbuild_chunks *out, const char *s, size_t len) {
    uint32_t lv[7][2] = {{0}}; /* heading text range per level */
    if (!new_section(x, out, s, 0, lv)) {
        return false;
    }
    for (size_t a = 0; a < len;) {
        const char *nl = memchr(s + a, '\n', len - a);
        size_t b = nl ? (size_t)(nl - s) : len;
        bool ok;
        if (b - a >= 2 && s[a] == ARCPACK_MARK_HEADING && s[a + 1] >= '1' && s[a + 1] <= '6') {
            int level = s[a + 1] - '0';
            lv[level][0] = (uint32_t)a + 2;
            lv[level][1] = (uint32_t)b;
            for (int l = level + 1; l <= 6; l++) {
                lv[l][0] = lv[l][1] = 0;
            }
            ok = new_section(x, out, s, (uint32_t)a, lv) && add_words(x, s, a + 2, b, (uint32_t)a, (uint32_t)b);
        } else if (b > a && s[a] == ARCPACK_MARK_IMAGE) {
            const char *sep = memchr(s + a, ARCPACK_MARK_SEP, b - a);
            ok = sep == NULL || add_words(x, s, (size_t)(sep - s) + 1, b, (uint32_t)a, (uint32_t)b);
        } else {
            ok = ordinary_line(x, s, a, b);
        }
        if (!ok || (nl && !add_mark(&x->line, &x->nl, &x->cl, (uint32_t)b + 1, (uint32_t)x->nw))) {
            return false;
        }
        a = b + 1;
    }
    return true;
}

/* last_at_most: index of the last mark with words <= n, or -1. */
static long last_at_most(const mark *m, size_t count, uint32_t n) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m[mid].words <= n) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (long)lo - 1;
}

/* chunk_add: append chunk [start, end) with `words` words and section sc's path. */
static bool chunk_add(packbuild_chunks *out, const section *sc, uint32_t start, uint32_t end, uint32_t words) {
    if (!grow_array(&out->chunk, out->n, &out->cap, sizeof(*out->chunk))) {
        return false;
    }
    out->chunk[out->n++] = (packbuild_chunk){start, end - start, words, sc->path_off, sc->path_len};
    return true;
}

/*
 * cut: where the chunk starting at word s (which ends a run whose words end at w1) stops: *pos the byte,
 * *wb the words before it. The run is longer than max words from s.
 */
static void cut(const struct packbuild_scratch *x, const packbuild_chunking *o, uint32_t s, uint32_t *pos,
                uint32_t *wb) {
    uint32_t lo = s + o->target - o->target / 4;
    uint32_t tgt = s + o->target;
    uint32_t hi = s + o->max;
    long i = last_at_most(x->line, x->nl, tgt);
    if (i >= 0 && x->line[i].words >= lo) {
        *pos = x->line[i].pos;
        *wb = x->line[i].words;
        return;
    }
    long below = last_at_most(x->sent, x->ns, tgt);
    long above = below + 1;
    bool b_ok = below >= 0 && x->sent[below].words >= lo;
    bool a_ok = (size_t)above < x->ns && x->sent[above].words <= hi;
    if (b_ok || a_ok) {
        long pick = !a_ok || (b_ok && tgt - x->sent[below].words <= x->sent[above].words - tgt) ? below : above;
        *pos = x->sent[pick].pos;
        *wb = x->sent[pick].words;
        return;
    }
    uint32_t k = tgt - 1; /* last word of the chunk, then the whole construct it lies in */
    if (x->w[k].clast + 1 - s > o->max && x->w[k].cfirst > s) {
        k = x->w[k].cfirst - 1; /* cut before a long construct */
    } else {
        k = x->w[k].clast; /* a construct longer than max alone at the start stays whole */
    }
    *pos = x->w[k].cend;
    *wb = k + 1;
}

/* window_start: the byte where a chunk starting at word s begins; moves *cur to the section holding s. */
static uint32_t window_start(const struct packbuild_scratch *x, size_t j, size_t *cur, uint32_t s) {
    while (*cur + 1 <= j && x->sec[*cur + 1].first <= s) {
        (*cur)++;
    }
    const section *sc = &x->sec[*cur];
    return sc->first == s && sc->start < x->w[s].cstart ? sc->start : x->w[s].cstart;
}

/*
 * run: chunks of sections [i, j] (bytes up to r1). A link, heading or image line longer than max words is never
 * cut: it becomes one chunk of its own, and the window before it ends where it starts.
 */
static bool run(struct packbuild_scratch *x, packbuild_chunks *out, const packbuild_chunking *o, size_t i, size_t j,
                uint32_t r1) {
    uint32_t w1 = j + 1 < x->nsec ? x->sec[j + 1].first : (uint32_t)x->nw;
    uint32_t s = x->sec[i].first;
    uint32_t start = x->sec[i].start;
    uint32_t done = s; /* words before the end of the last chunk */
    size_t cur = i;    /* section holding word s (the run's first section for its first chunk) */
    for (;;) {
        if (w1 - s <= o->max) {
            return chunk_add(out, &x->sec[cur], start, r1, w1 - s);
        }
        uint32_t pos;
        uint32_t wb;
        cut(x, o, s, &pos, &wb);
        if (wb <= done) {
            /* The overlap ran into a construct too long to share: no overlap, start at the construct. */
            s = done;
            start = window_start(x, j, &cur, s);
            continue;
        }
        if (!chunk_add(out, &x->sec[cur], start, pos, wb - s)) {
            return false;
        }
        done = wb;
        if (wb == w1) {
            return true; /* a construct longer than max ended the run */
        }
        uint32_t ns = wb > s + o->overlap ? wb - o->overlap : s + 1;
        ns = x->w[ns].cfirst > s ? x->w[ns].cfirst : (x->w[ns].clast + 1 < wb ? x->w[ns].clast + 1 : wb);
        size_t at = cur; /* the overlap never reaches back over a heading */
        while (at + 1 <= j && x->sec[at + 1].first <= wb) {
            at++;
        }
        if (ns < x->sec[at].first) {
            ns = x->sec[at].first;
        }
        s = ns;
        start = window_start(x, j, &cur, s);
    }
}

bool packbuild_chunk_text(const char *text, size_t len, const packbuild_chunking *opt, packbuild_chunks *out) {
    out->n = 0;
    out->paths_len = 0;
    if (out->scratch == NULL && (out->scratch = calloc(1, sizeof(*out->scratch))) == NULL) {
        return false;
    }
    struct packbuild_scratch *x = out->scratch;
    x->nw = x->nl = x->ns = x->nsec = 0;
    if (len == 0) {
        return true;
    }
    if (!scan_text(x, out, text, len)) {
        return false;
    }
    for (size_t i = 0; i < x->nsec;) {
        size_t j = i;
        uint32_t w0 = x->sec[i].first;
        while (j + 1 < x->nsec && x->sec[j + 1].first - w0 < opt->min_section) {
            j++; /* a short section joins the next one */
        }
        uint32_t w1 = j + 1 < x->nsec ? x->sec[j + 1].first : (uint32_t)x->nw;
        uint32_t r1 = j + 1 < x->nsec ? x->sec[j + 1].start : (uint32_t)len;
        if (w1 > w0 && !run(x, out, opt, i, j, r1)) {
            return false;
        }
        i = j + 1;
    }
    return true;
}

void packbuild_chunks_free(packbuild_chunks *c) {
    if (c->scratch != NULL) {
        free(c->scratch->w);
        free(c->scratch->line);
        free(c->scratch->sent);
        free(c->scratch->sec);
        free(c->scratch);
    }
    free(c->chunk);
    free(c->paths);
    memset(c, 0, sizeof(*c));
}
