/* arc_os — Host-side tests for kernel/lib/crc32.c, kernel/diag/qr.c and
 * kernel/diag/qr_text.c (M1-04b).
 *
 * Known-answer tests check the tables against ISO/IEC 18004; the round trip
 * renders each code to a PGM and decodes it with zbarimg (zbar-tools, the
 * reference decoder on the build host), which must give back the input byte
 * for byte. ksnprintf comes from test_kprintf.c, which compiles kprintf.c. */

#define _POSIX_C_SOURCE 200809L   /* mkstemp, popen */

#include "test_framework.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../kernel/lib/crc32.c"
#include "../kernel/diag/qr.c"
#include "../kernel/diag/qr_text.c"
#include "qr_report_sample.h"

static QrCode qr;   /* about 7 KiB: static, as in the kernel */

/* ---- Helpers ---- */

/* Fixed-seed generator so every run tests the same inputs */
static uint32_t rng_state;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static void random_text(uint8_t *buf, size_t len, uint32_t seed) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:;-_[]=\n";
    rng_state = seed;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)alphabet[rng_next() % (sizeof(alphabet) - 1)];
}

/* Write qr as a binary PGM: 4 pixels per module, 4-module light quiet zone. */
static int write_pgm(const QrCode *code, const char *path) {
    const int scale = 4, border = 4;
    int w = (code->size + 2 * border) * scale;
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    fprintf(f, "P5\n%d %d\n255\n", w, w);
    for (int py = 0; py < w; py++) {
        for (int px = 0; px < w; px++) {
            fputc(qr_module(code, px / scale - border, py / scale - border) ? 0 : 255, f);
        }
    }
    return fclose(f);
}

static int zbar_checked;

/* Decode `code` with zbarimg into out; returns the byte count, or -1 if
 * nothing was decoded. */
static long zbar_decode(const QrCode *code, uint8_t *out, size_t cap) {
    if (!zbar_checked) {
        if (system("command -v zbarimg >/dev/null 2>&1") != 0) {
            printf("  FAIL: zbarimg not found: run tools/dev-setup.sh apt (zbar-tools)\n");
            return -1;
        }
        zbar_checked = 1;
    }
    char path[] = "/tmp/arc_qr_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    if (write_pgm(code, path) != 0) {
        unlink(path);
        return -1;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "zbarimg --raw -q -Sbinary %s 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    size_t n = 0;
    if (p != NULL) {
        n = fread(out, 1, cap, p);
        if (pclose(p) != 0) n = (size_t)-1;
    }
    unlink(path);
    return p == NULL ? -1 : (long)n;
}

static uint8_t decoded[4096];

/* Encode data (up to max_version), decode it, compare. Returns the version, or -1. */
static int roundtrip(const uint8_t *data, size_t len, int max_version) {
    int ver = qr_encode(data, len, max_version, &qr);
    if (ver < 1) {
        printf("  FAIL: qr_encode(%zu bytes) = %d\n", len, ver);
        return -1;
    }
    long got = zbar_decode(&qr, decoded, sizeof(decoded));
    if (got != (long)len || (len > 0 && memcmp(decoded, data, len) != 0)) {
        printf("  FAIL: round trip of %zu bytes at version %d mask %d: decoded %ld bytes\n", len, ver, qr.mask, got);
        return -1;
    }
    return ver;
}

/* ---- CRC-32 ---- */

TEST(crc32_check_value) {
    ASSERT_EQ(crc32(0, "123456789", 9), 0xCBF43926u);
    ASSERT_EQ(crc32(0, "", 0), 0);
    ASSERT_EQ(crc32(0, "a", 1), 0xE8B7BE43u);
    /* Chaining equals one pass */
    ASSERT_EQ(crc32(crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
    return 0;
}

/* ---- GF(256) and Reed-Solomon ---- */

TEST(gf_tables) {
    /* gf_exp[i] = 2^i by repeated doubling mod 0x11D; gf_log is its inverse */
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        ASSERT_EQ(gf_exp[i], x);
        ASSERT_EQ(gf_log[x], i);
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    ASSERT_EQ(x, 1);   /* 2 generates the whole group: order 255 */
    for (int a = 1; a < 256; a++) {
        ASSERT_EQ(gf_mul((uint8_t)a, 0x37), gf_mul(0x37, (uint8_t)a));        /* commutative */
        ASSERT_EQ(gf_mul((uint8_t)a, gf_exp[(255 - gf_log[a]) % 255]), 1);  /* inverse */
    }
    ASSERT_EQ(gf_mul(2, 0x80), 0x1D);   /* x * x^7 = x^8 = x^4 + x^3 + x^2 + 1 */
    ASSERT_EQ(gf_mul(0, 0x55), 0);
    return 0;
}

/* Every block's data + ECC is a codeword: it evaluates to 0 at 2^0 .. 2^(ecc-1). */
static int rs_syndromes_zero(const QrCode *code) {
    BlockLayout b = block_layout(code->version);
    uint8_t block[256];
    for (int blk = 0; blk < b.nblocks; blk++) {
        int dlen = b.short_len + (blk >= b.nshort);
        memcpy(block, code->codewords + block_start(&b, blk), (size_t)dlen);
        memcpy(block + dlen, code->codewords + b.ndata + blk * b.ecc, (size_t)b.ecc);
        for (int i = 0; i < b.ecc; i++) {
            uint8_t s = 0;
            for (int j = 0; j < dlen + b.ecc; j++) s = gf_mul(s, gf_exp[i]) ^ block[j];
            if (s != 0) return 0;
        }
    }
    return 1;
}

TEST(rs_known_answer) {
    /* ISO/IEC 18004 annex I example "01234567" (1-M): data and ECC codewords */
    static const uint8_t data[16] = {0x10, 0x20, 0x0C, 0x56, 0x61, 0x80, 0xEC, 0x11,
                                     0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11};
    static const uint8_t ecc[10] = {0xA5, 0x24, 0xD4, 0xC1, 0xED, 0x36, 0xC7, 0x87, 0x2C, 0x55};
    uint8_t div[10], out[10];
    rs_divisor(10, div);
    rs_remainder(data, 16, div, 10, out);
    ASSERT_MEM_EQ(out, ecc, 10);
    return 0;
}

TEST(rs_syndromes_every_version) {
    uint8_t buf[QR_BYTES_MAX];
    for (int v = 1; v <= QR_VERSION_MAX; v++) {
        random_text(buf, qr_capacity(v), 1000u + (uint32_t)v);
        ASSERT_EQ(qr_encode(buf, qr_capacity(v), v, &qr), v);
        ASSERT_TRUE(rs_syndromes_zero(&qr));
    }
    return 0;
}

/* ---- Format and version information ---- */

TEST(format_words) {
    /* ISO/IEC 18004 table C.1, rows L, M, Q, H (level bits 01, 00, 11, 10), masks 0-7 */
    static const uint16_t table[4][8] = {
        {0x77C4, 0x72F3, 0x7DAA, 0x789D, 0x662F, 0x6318, 0x6C41, 0x6976},
        {0x5412, 0x5125, 0x5E7C, 0x5B4B, 0x45F9, 0x40CE, 0x4F97, 0x4AA0},
        {0x355F, 0x3068, 0x3F31, 0x3A06, 0x24B4, 0x2183, 0x2EDA, 0x2BED},
        {0x1689, 0x13BE, 0x1CE7, 0x19D0, 0x0762, 0x0255, 0x0D0C, 0x083B},
    };
    static const uint32_t ecl[4] = {1, 0, 3, 2};
    for (int l = 0; l < 4; l++) {
        for (int m = 0; m < 8; m++) ASSERT_EQ(format_bits(ecl[l], m), table[l][m]);
    }
    return 0;
}

TEST(version_words) {
    /* ISO/IEC 18004 table D.1, versions 7-30 */
    static const uint32_t table[24] = {
        0x07C94, 0x085BC, 0x09A99, 0x0A4D3, 0x0BBF6, 0x0C762, 0x0D847, 0x0E60D, 0x0F928, 0x10B78, 0x1145D, 0x12A17,
        0x13532, 0x149A6, 0x15683, 0x168C9, 0x177EC, 0x18EC4, 0x191E1, 0x1AFAB, 0x1B08E, 0x1CC1A, 0x1D33F, 0x1ED75,
    };
    for (int v = 7; v <= 30; v++) ASSERT_EQ(version_bits(v), table[v - 7]);
    return 0;
}

TEST(capacity_table) {
    /* ISO/IEC 18004 table 7: byte-mode capacity at level M, versions 1-30 */
    static const uint16_t table[30] = {
        14,  26,  42,  62,  84,  106, 122, 152, 180, 213, 251,  287,  331,  362,  412,
        450, 504, 560, 624, 666, 711, 779, 857, 911, 997, 1059, 1125, 1190, 1264, 1370,
    };
    for (int v = 1; v <= 30; v++) ASSERT_EQ(qr_capacity(v), table[v - 1]);
    ASSERT_EQ(qr_capacity(30), QR_BYTES_MAX);
    ASSERT_EQ(raw_codewords(30), QR_CODEWORDS_MAX);
    ASSERT_EQ(qr_capacity(0), 0);
    ASSERT_EQ(qr_capacity(31), 0);
    /* Data codewords of 1-M and 30-M (table 9) */
    ASSERT_EQ(data_codewords(1), 16);
    ASSERT_EQ(data_codewords(30), 1373);
    return 0;
}

TEST(alignment_positions) {
    /* ISO/IEC 18004 annex E, a few rows */
    int pos[7];
    ASSERT_EQ(alignment_positions(1, pos), 0);
    ASSERT_EQ(alignment_positions(2, pos), 2);
    ASSERT_EQ(pos[1], 18);
    ASSERT_EQ(alignment_positions(7, pos), 3);
    ASSERT_TRUE(pos[0] == 6 && pos[1] == 22 && pos[2] == 38);
    ASSERT_EQ(alignment_positions(15, pos), 4);
    ASSERT_TRUE(pos[1] == 26 && pos[2] == 48 && pos[3] == 70);
    ASSERT_EQ(alignment_positions(30, pos), 6);
    ASSERT_TRUE(pos[0] == 6 && pos[1] == 26 && pos[2] == 52 && pos[3] == 78 && pos[4] == 104 && pos[5] == 130);
    return 0;
}

/* ---- Mask penalty ---- */

static void set_row(QrCode *code, int y, const char *bits) {
    for (int x = 0; bits[x] != '\0'; x++) bit_put(code->modules, x, y, bits[x] == '1');
}

TEST(penalty_rule3_finder_like) {
    memset(&qr, 0, sizeof(qr));
    qr.size = 21;
    /* 1011101 with 4 light on both sides: N3 twice; the trailing light run of 10 adds N1 + 5 */
    set_row(&qr, 0, "000010111010000000000");
    ASSERT_EQ(penalty_line(&qr, 1, 0), 2 * PENALTY_N3 + PENALTY_N1 + 5);
    /* At the left edge the quiet zone is the light side: N3 once from the left,
     * once from the right (4 light after), light run of 14: N1 + 9 */
    set_row(&qr, 1, "101110100000000000000");
    ASSERT_EQ(penalty_line(&qr, 1, 1), 2 * PENALTY_N3 + PENALTY_N1 + 9);
    /* A run of 5 inside the line costs N1, nothing else here */
    set_row(&qr, 3, "111110101010101010101");
    ASSERT_EQ(penalty_line(&qr, 1, 3), PENALTY_N1);
    /* Dark on both sides within 4 modules: no finder-like pattern */
    set_row(&qr, 2, "101011101010101010101");
    ASSERT_EQ(penalty_line(&qr, 1, 2), 0);
    /* Columns read the same way: column 0 holds rows 0-20 = row 0's bits */
    memset(qr.modules, 0, sizeof(qr.modules));
    const char *col = "000010111010000000000";
    for (int y = 0; y < 21; y++) bit_put(qr.modules, 0, y, col[y] == '1');
    ASSERT_EQ(penalty_line(&qr, 0, 0), 2 * PENALTY_N3 + PENALTY_N1 + 5);
    return 0;
}

TEST(penalty_rules_1_2_4) {
    memset(&qr, 0, sizeof(qr));
    qr.size = 21;
    /* All light: rule 1 = 21 rows + 21 columns of run 21 (N1 + 16 each);
     * rule 2 = 20 x 20 blocks; rule 4 = 0% dark, 10 steps of 5% away: 9 * N4 */
    ASSERT_EQ(penalty_line(&qr, 1, 0), PENALTY_N1 + 16);
    ASSERT_EQ(penalty_blocks(&qr), 400 * PENALTY_N2);
    ASSERT_EQ(penalty_balance(&qr), 9 * PENALTY_N4);
    /* One dark module at (0, 1) breaks the two blocks that hold it */
    bit_put(qr.modules, 0, 1, 1);
    ASSERT_EQ(penalty_blocks(&qr), 398 * PENALTY_N2);
    /* Checkerboard: no runs, no blocks, 221 of 441 dark (50.1%): 0 */
    for (int y = 0; y < 21; y++) {
        for (int x = 0; x < 21; x++) bit_put(qr.modules, x, y, (x + y) % 2 == 0);
    }
    ASSERT_EQ(penalty_line(&qr, 1, 3), 0);
    ASSERT_EQ(penalty_blocks(&qr), 0);
    ASSERT_EQ(penalty_balance(&qr), 0);
    return 0;
}

/* The mask kept is the first one with the lowest total penalty */
TEST(mask_choice_is_lowest_penalty) {
    static uint8_t buf[QR_BYTES_MAX];
    for (int v = 1; v <= QR_VERSION_MAX; v += 3) {
        random_text(buf, qr_capacity(v), 500u + (uint32_t)v);
        ASSERT_EQ(qr_encode(buf, qr_capacity(v), 30, &qr), v);
        int chosen = qr.mask, scores[8];
        apply_mask(&qr, chosen);
        for (int m = 0; m < 8; m++) {
            apply_mask(&qr, m);
            draw_format(&qr, m);
            scores[m] = penalty(&qr);
            apply_mask(&qr, m);
        }
        for (int m = 0; m < 8; m++) {
            ASSERT_TRUE(scores[chosen] < scores[m] || (scores[chosen] == scores[m] && chosen <= m));
        }
    }
    return 0;
}

/* ---- Encoder arguments ---- */

TEST(encode_arguments) {
    uint8_t b = 'x';
    ASSERT_EQ(qr_encode(&b, 1, 0, &qr), -EINVAL);
    ASSERT_EQ(qr_encode(&b, 1, 31, &qr), -EINVAL);
    ASSERT_EQ(qr_encode(&b, 1, 30, NULL), -EINVAL);
    ASSERT_EQ(qr_encode(NULL, 1, 30, &qr), -EINVAL);
    ASSERT_EQ(qr_encode(NULL, 0, 30, &qr), 1);
    ASSERT_EQ(qr.size, 21);
    return 0;
}

/* ---- Round trip through zbarimg ---- */

TEST(roundtrip_empty_and_one_byte) {
    ASSERT_EQ(roundtrip((const uint8_t *)"", 0, 30), 1);
    ASSERT_EQ(roundtrip((const uint8_t *)"A", 1, 30), 1);
    return 0;
}

/* Exactly the capacity of every version picks that version and decodes;
 * one byte more needs the next version (-E2BIG when capped at this one). */
TEST(roundtrip_capacity_every_version) {
    static uint8_t buf[QR_BYTES_MAX + 1];
    for (int v = 1; v <= QR_VERSION_MAX; v++) {
        size_t cap = qr_capacity(v);
        random_text(buf, cap + 1, 7u * (uint32_t)v);
        ASSERT_EQ(roundtrip(buf, cap, 30), v);
        ASSERT_EQ(qr_encode(buf, cap + 1, v, &qr), -E2BIG);
        if (v < QR_VERSION_MAX) ASSERT_EQ(qr_encode(buf, cap + 1, 30, &qr), v + 1);
    }
    ASSERT_EQ(qr_encode(buf, QR_BYTES_MAX + 1, 30, &qr), -E2BIG);
    return 0;
}

TEST(roundtrip_all_byte_values) {
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)i;
    ASSERT_TRUE(roundtrip(buf, sizeof(buf), 30) > 0);
    return 0;
}

TEST(roundtrip_random) {
    static uint8_t buf[QR_BYTES_MAX];
    static const size_t lens[] = {2, 17, 100, 333, 777, 1200};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        random_text(buf, lens[i], 42u + (uint32_t)i);
        ASSERT_TRUE(roundtrip(buf, lens[i], 30) > 0);
    }
    /* Random binary, not just text */
    rng_state = 99;
    for (size_t i = 0; i < 500; i++) buf[i] = (uint8_t)rng_next();
    ASSERT_TRUE(roundtrip(buf, 500, 30) > 0);
    return 0;
}

/* The penalty picks the mask, so some masks may never be chosen above: force
 * each of the 8 onto one version-7 code (version bits too) and decode it. */
TEST(roundtrip_every_mask) {
    uint8_t buf[110];   /* version 6 holds 106 */
    random_text(buf, sizeof(buf), 5);
    ASSERT_EQ(qr_encode(buf, sizeof(buf), 30, &qr), 7);
    for (int m = 0; m < 8; m++) {
        apply_mask(&qr, qr.mask);   /* undo the chosen mask */
        apply_mask(&qr, m);
        draw_format(&qr, m);
        qr.mask = m;
        long got = zbar_decode(&qr, decoded, sizeof(decoded));
        ASSERT_EQ(got, (long)sizeof(buf));
        ASSERT_MEM_EQ(decoded, buf, sizeof(buf));
    }
    return 0;
}

/* ---- Exact symbol checks ----
 * A decoder repairs a few wrong modules (Reed-Solomon in the data, BCH and a
 * second copy for the format word), so a round trip alone can hide a small
 * placement bug. These read the finished symbol back by the standard's
 * layout, written out separately here, and demand an exact match. */

/* Format bit 14 (most significant) .. 0, as (row, column) in ISO/IEC 18004
 * figure 25; negative values count from the far edge (-1 = size - 1). */
static const int8_t format_copy1[15][2] = {
    {8, 0}, {8, 1}, {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 7}, {8, 8},
    {7, 8}, {5, 8}, {4, 8}, {3, 8}, {2, 8}, {1, 8}, {0, 8},
};
static const int8_t format_copy2[15][2] = {
    {-1, 8}, {-2, 8}, {-3, 8}, {-4, 8}, {-5, 8}, {-6, 8}, {-7, 8}, {8, -8},
    {8, -7}, {8, -6}, {8, -5}, {8, -4}, {8, -3}, {8, -2}, {8, -1},
};

static uint32_t read_format(const QrCode *code, const int8_t pos[15][2]) {
    uint32_t w = 0;
    for (int i = 0; i < 15; i++) {
        int r = pos[i][0] < 0 ? code->size + pos[i][0] : pos[i][0];
        int c = pos[i][1] < 0 ? code->size + pos[i][1] : pos[i][1];
        w = (w << 1) | (uint32_t)qr_module(code, c, r);
    }
    return w;
}

/* Mask condition, ISO/IEC 18004 table 10 (i = row, j = column) */
static int std_mask(int m, int i, int j) {
    switch (m) {
    case 0:
        return (i + j) % 2 == 0;
    case 1:
        return i % 2 == 0;
    case 2:
        return j % 3 == 0;
    case 3:
        return (i + j) % 3 == 0;
    case 4:
        return ((i / 2) + (j / 3)) % 2 == 0;
    case 5:
        return (i * j) % 2 + (i * j) % 3 == 0;
    case 6:
        return ((i * j) % 2 + (i * j) % 3) % 2 == 0;
    default:
        return ((i * j) % 3 + (i + j) % 2) % 2 == 0;
    }
}

/* Read every data bit in placement order (two-column zigzag from the bottom
 * right, the timing column skipped), unmask it, and compare with the
 * interleaved codeword stream. Returns the number of mismatched bits. */
static int reread_mismatches(const QrCode *code) {
    BlockLayout b = block_layout(code->version);
    int total = raw_codewords(code->version) * 8, k = 0, bad = 0, up = 1;
    for (int col = code->size - 1; col > 0; col -= 2, up = !up) {
        if (col == 6) col--;
        for (int step = 0; step < code->size; step++) {
            int row = up ? code->size - 1 - step : step;
            for (int c = col; c >= col - 1; c--) {
                if (bit_get(code->function, c, row)) continue;
                int bit = qr_module(code, c, row) ^ std_mask(code->mask, row, c);
                int want = k < total ? (interleaved(code, &b, k >> 3) >> (7 - (k & 7))) & 1 : 0;
                bad += bit != want;
                k++;
            }
        }
    }
    return bad + (k < total ? total - k : 0);
}

TEST(symbol_exact_format_and_data) {
    static uint8_t buf[QR_BYTES_MAX];
    for (int v = 1; v <= QR_VERSION_MAX; v++) {
        random_text(buf, qr_capacity(v), 300u + (uint32_t)v);
        ASSERT_EQ(qr_encode(buf, qr_capacity(v), 30, &qr), v);
        uint32_t want = format_bits(QR_ECL_M, qr.mask);
        ASSERT_EQ(read_format(&qr, format_copy1), want);
        ASSERT_EQ(read_format(&qr, format_copy2), want);
        ASSERT_EQ(qr_module(&qr, 8, qr.size - 8), 1);   /* the dark module */
        ASSERT_EQ(reread_mismatches(&qr), 0);
    }
    return 0;
}

/* ---- Text pages ---- */

static const QrTextFormat report_fmt = {"1ad2f13", "report", 30};

TEST(sanitize) {
    char out[64];
    const char *in = "ok\tA\x1b[0m\xff\n";
    size_t n = qr_text_sanitize((const uint8_t *)in, strlen(in), out, sizeof(out));
    const char *want = "ok\\x09A\\x1b[0m\\xff\n";
    ASSERT_EQ(n, strlen(want));
    ASSERT_MEM_EQ(out, want, n);

    in = "see http://x.example and file:///a, a:/b, :/";
    n = qr_text_sanitize((const uint8_t *)in, strlen(in), out, sizeof(out));
    want = "see http:\\/\\/x.example and file:\\/\\//a, a:/b, :/";
    ASSERT_EQ(n, strlen(want));
    ASSERT_MEM_EQ(out, want, n);

    /* A NUL byte is escaped, not a terminator */
    n = qr_text_sanitize((const uint8_t *)"a\0b", 3, out, sizeof(out));
    ASSERT_EQ(n, 6);
    ASSERT_MEM_EQ(out, "a\\x00b", 6);

    /* Truncation keeps whole escapes and reports the full length */
    memset(out, '#', sizeof(out));
    n = qr_text_sanitize((const uint8_t *)"ab\x01", 3, out, 4);
    ASSERT_EQ(n, 6);
    ASSERT_MEM_EQ(out, "ab##", 4);
    return 0;
}

TEST(paginate_arguments) {
    QrTextPage pages[4];
    QrTextFormat bad = report_fmt;
    bad.commit = "has space";
    ASSERT_EQ(qr_text_paginate(&bad, "x", 1, pages, 4), -EINVAL);
    bad = report_fmt;
    bad.kind = "";
    ASSERT_EQ(qr_text_paginate(&bad, "x", 1, pages, 4), -EINVAL);
    bad = report_fmt;
    bad.max_version = 31;
    ASSERT_EQ(qr_text_paginate(&bad, "x", 1, pages, 4), -EINVAL);
    /* Version 1 holds 14 bytes: no room for a header */
    bad = report_fmt;
    bad.max_version = 1;
    ASSERT_EQ(qr_text_paginate(&bad, "x", 1, pages, 4), -EINVAL);
    ASSERT_EQ(qr_text_paginate(&report_fmt, "x", 1, pages, 0), -EINVAL);

    /* Empty text: one empty page with both CRCs 0 */
    ASSERT_EQ(qr_text_paginate(&report_fmt, "", 0, pages, 4), 1);
    char out[QR_BYTES_MAX];
    int n = qr_text_page(&report_fmt, "", pages, 1, 0, out, sizeof(out));
    const char *want = "ARC1 1ad2f13 report 1/1 00000000\nall 00000000\n";
    ASSERT_EQ(n, (int)strlen(want));
    ASSERT_MEM_EQ(out, want, (size_t)n);
    ASSERT_EQ(qr_text_page(&report_fmt, "", pages, 1, 1, out, sizeof(out)), -EINVAL);
    ASSERT_EQ(qr_text_page(&report_fmt, "", pages, 1, 0, out, 10), -E2BIG);
    return 0;
}

/* Check pages cover the text in order, each payload fits, carries the right
 * header and CRCs, and decodes back from a code. */
static int check_pages(const QrTextFormat *fmt, const char *text, size_t len, const QrTextPage *pages, int n,
                       int decode) {
    static char out[QR_BYTES_MAX];
    char want[128];
    size_t pos = 0;
    uint32_t all = 0;
    for (int i = 0; i < n; i++) {
        ASSERT_EQ(pages[i].start, pos);
        pos += pages[i].len;
        const char *body = text + pages[i].start;
        all = crc32(all, body, pages[i].len);
        int plen = qr_text_page(fmt, text, pages, n, i, out, sizeof(out));
        ASSERT_TRUE(plen > 0 && (size_t)plen <= qr_capacity(fmt->max_version));
        int hlen = snprintf(want, sizeof(want), "ARC1 %s %s %d/%d %08x\n", fmt->commit, fmt->kind, i + 1, n,
                            crc32(0, body, pages[i].len));
        ASSERT_MEM_EQ(out, want, (size_t)hlen);
        ASSERT_MEM_EQ(out + hlen, body, pages[i].len);
        size_t rest = (size_t)plen - (size_t)hlen - pages[i].len;
        if (i == n - 1) {
            int nl = pages[i].len > 0 && body[pages[i].len - 1] != '\n';
            int alen = snprintf(want, sizeof(want), "%sall %08x\n", nl ? "\n" : "", all);
            ASSERT_EQ(rest, (size_t)alen);
            ASSERT_MEM_EQ(out + hlen + pages[i].len, want, rest);
        } else {
            ASSERT_EQ(rest, 0);
        }
        if (decode) {
            ASSERT_TRUE(roundtrip((const uint8_t *)out, (size_t)plen, fmt->max_version) > 0);
        }
    }
    ASSERT_EQ(pos, len);
    ASSERT_EQ(all, crc32(0, text, len));
    return 0;
}

TEST(paginate_lines_and_long_line) {
    static char text[6000];
    QrTextPage pages[16];
    QrTextFormat fmt = {"unknown", "log", 10};   /* 213 bytes per code */
    /* Short lines, a 700-byte line with no newline in it, then short lines */
    size_t len = 0;
    for (int i = 0; i < 12; i++) len += (size_t)snprintf(text + len, 64, "[%4d.000] line %d\n", i, i);
    size_t long_start = len;
    for (int i = 0; i < 700; i++) text[len++] = (char)('a' + i % 26);
    text[len++] = '\n';
    for (int i = 0; i < 5; i++) len += (size_t)snprintf(text + len, 64, "tail %d\n", i);
    text[len] = '\0';

    int n = qr_text_paginate(&fmt, text, len, pages, 16);
    ASSERT_TRUE(n > 4);
    /* Pages before the long line end at line ends */
    for (int i = 0; i < n && pages[i].start + pages[i].len <= long_start; i++) {
        ASSERT_EQ(text[pages[i].start + pages[i].len - 1], '\n');
    }
    ASSERT_EQ(check_pages(&fmt, text, len, pages, n, 1), 0);
    /* Too few pages */
    ASSERT_EQ(qr_text_paginate(&fmt, text, len, pages, 2), -E2BIG);
    /* Text without a final newline: the "all" line gets its own */
    n = qr_text_paginate(&fmt, "abc", 3, pages, 16);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(check_pages(&fmt, "abc", 3, pages, n, 1), 0);
    return 0;
}

/* A real boot report from QEMU, sanitized and paged at version 30 */
TEST(paginate_qemu_report) {
    static char clean[8192];
    QrTextPage pages[8];
    size_t len = qr_text_sanitize((const uint8_t *)qr_report_sample, strlen(qr_report_sample), clean, sizeof(clean));
    ASSERT_TRUE(len <= sizeof(clean));
    ASSERT_EQ(len, strlen(qr_report_sample));   /* the report is already plain ASCII */
    int n = qr_text_paginate(&report_fmt, clean, len, pages, 8);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(check_pages(&report_fmt, clean, len, pages, n, 1), 0);
    return 0;
}

TestCase qr_tests[] = {
    TEST_ENTRY(crc32_check_value),
    TEST_ENTRY(gf_tables),
    TEST_ENTRY(rs_known_answer),
    TEST_ENTRY(rs_syndromes_every_version),
    TEST_ENTRY(format_words),
    TEST_ENTRY(version_words),
    TEST_ENTRY(capacity_table),
    TEST_ENTRY(alignment_positions),
    TEST_ENTRY(penalty_rule3_finder_like),
    TEST_ENTRY(penalty_rules_1_2_4),
    TEST_ENTRY(mask_choice_is_lowest_penalty),
    TEST_ENTRY(encode_arguments),
    TEST_ENTRY(roundtrip_empty_and_one_byte),
    TEST_ENTRY(roundtrip_capacity_every_version),
    TEST_ENTRY(roundtrip_all_byte_values),
    TEST_ENTRY(roundtrip_random),
    TEST_ENTRY(roundtrip_every_mask),
    TEST_ENTRY(symbol_exact_format_and_data),
    TEST_ENTRY(sanitize),
    TEST_ENTRY(paginate_arguments),
    TEST_ENTRY(paginate_lines_and_long_line),
    TEST_ENTRY(paginate_qemu_report),
};

int qr_test_count = sizeof(qr_tests) / sizeof(qr_tests[0]);
