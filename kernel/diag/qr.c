/* qr.c — QR Code encoder: byte mode, level M, versions 1-30 (M1-04b).
 *
 * Owns: turning bytes into a module bitmap, following ISO/IEC 18004.
 *   1. Data: mode indicator 0100 (byte), the length (8 bits for versions 1-9,
 *      16 bits from 10), the bytes, a terminator, then pad bytes 0xEC 0x11.
 *   2. Error correction: the data codewords are cut into blocks and each block
 *      gets Reed-Solomon codewords over GF(256) (polynomial 0x11D), so a
 *      reader can repair about 15% damage (level M).
 *   3. Placement: finders, timing lines, alignment patterns, format and
 *      version information are drawn first ("function modules"); the
 *      interleaved codewords then fill the rest in a two-column zigzag.
 *   4. Masking: each of the 8 masks flips data modules by a fixed pattern; the
 *      one with the lowest penalty (long runs, 2x2 blocks, finder look-alikes,
 *      dark/light imbalance) is kept so readers lock onto the code easily.
 *
 * Invariants: everything lives in the caller's QrCode; no allocation, no
 * globals besides const tables, no locks, no floating point. Every table value
 * is proven by the decoder round trip in tests/test_qr.c. */

#include "diag/qr.h"
#include "lib/mem.h"
#include "uapi/errno.h"

/* ---- Tables (ISO/IEC 18004 table 9, level M, index = version) ---- */

static const uint8_t ecc_per_block[QR_VERSION_MAX + 1] = {
    0,  10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24,
    28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28,
};

static const uint8_t num_blocks[QR_VERSION_MAX + 1] = {
    0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29,
};

/* GF(256) with polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D): gf_exp[i] = 2^i,
 * gf_log[gf_exp[i]] = i (gf_log[0] is unused). Generated. */
static const uint8_t gf_exp[256] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1d, 0x3a, 0x74, 0xe8, 0xcd, 0x87, 0x13, 0x26, 0x4c, 0x98, 0x2d,
    0x5a, 0xb4, 0x75, 0xea, 0xc9, 0x8f, 0x03, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0x9d, 0x27, 0x4e, 0x9c, 0x25, 0x4a,
    0x94, 0x35, 0x6a, 0xd4, 0xb5, 0x77, 0xee, 0xc1, 0x9f, 0x23, 0x46, 0x8c, 0x05, 0x0a, 0x14, 0x28, 0x50, 0xa0, 0x5d,
    0xba, 0x69, 0xd2, 0xb9, 0x6f, 0xde, 0xa1, 0x5f, 0xbe, 0x61, 0xc2, 0x99, 0x2f, 0x5e, 0xbc, 0x65, 0xca, 0x89, 0x0f,
    0x1e, 0x3c, 0x78, 0xf0, 0xfd, 0xe7, 0xd3, 0xbb, 0x6b, 0xd6, 0xb1, 0x7f, 0xfe, 0xe1, 0xdf, 0xa3, 0x5b, 0xb6, 0x71,
    0xe2, 0xd9, 0xaf, 0x43, 0x86, 0x11, 0x22, 0x44, 0x88, 0x0d, 0x1a, 0x34, 0x68, 0xd0, 0xbd, 0x67, 0xce, 0x81, 0x1f,
    0x3e, 0x7c, 0xf8, 0xed, 0xc7, 0x93, 0x3b, 0x76, 0xec, 0xc5, 0x97, 0x33, 0x66, 0xcc, 0x85, 0x17, 0x2e, 0x5c, 0xb8,
    0x6d, 0xda, 0xa9, 0x4f, 0x9e, 0x21, 0x42, 0x84, 0x15, 0x2a, 0x54, 0xa8, 0x4d, 0x9a, 0x29, 0x52, 0xa4, 0x55, 0xaa,
    0x49, 0x92, 0x39, 0x72, 0xe4, 0xd5, 0xb7, 0x73, 0xe6, 0xd1, 0xbf, 0x63, 0xc6, 0x91, 0x3f, 0x7e, 0xfc, 0xe5, 0xd7,
    0xb3, 0x7b, 0xf6, 0xf1, 0xff, 0xe3, 0xdb, 0xab, 0x4b, 0x96, 0x31, 0x62, 0xc4, 0x95, 0x37, 0x6e, 0xdc, 0xa5, 0x57,
    0xae, 0x41, 0x82, 0x19, 0x32, 0x64, 0xc8, 0x8d, 0x07, 0x0e, 0x1c, 0x38, 0x70, 0xe0, 0xdd, 0xa7, 0x53, 0xa6, 0x51,
    0xa2, 0x59, 0xb2, 0x79, 0xf2, 0xf9, 0xef, 0xc3, 0x9b, 0x2b, 0x56, 0xac, 0x45, 0x8a, 0x09, 0x12, 0x24, 0x48, 0x90,
    0x3d, 0x7a, 0xf4, 0xf5, 0xf7, 0xf3, 0xfb, 0xeb, 0xcb, 0x8b, 0x0b, 0x16, 0x2c, 0x58, 0xb0, 0x7d, 0xfa, 0xe9, 0xcf,
    0x83, 0x1b, 0x36, 0x6c, 0xd8, 0xad, 0x47, 0x8e, 0x01,
};

static const uint8_t gf_log[256] = {
    0x00, 0x00, 0x01, 0x19, 0x02, 0x32, 0x1a, 0xc6, 0x03, 0xdf, 0x33, 0xee, 0x1b, 0x68, 0xc7, 0x4b, 0x04, 0x64, 0xe0,
    0x0e, 0x34, 0x8d, 0xef, 0x81, 0x1c, 0xc1, 0x69, 0xf8, 0xc8, 0x08, 0x4c, 0x71, 0x05, 0x8a, 0x65, 0x2f, 0xe1, 0x24,
    0x0f, 0x21, 0x35, 0x93, 0x8e, 0xda, 0xf0, 0x12, 0x82, 0x45, 0x1d, 0xb5, 0xc2, 0x7d, 0x6a, 0x27, 0xf9, 0xb9, 0xc9,
    0x9a, 0x09, 0x78, 0x4d, 0xe4, 0x72, 0xa6, 0x06, 0xbf, 0x8b, 0x62, 0x66, 0xdd, 0x30, 0xfd, 0xe2, 0x98, 0x25, 0xb3,
    0x10, 0x91, 0x22, 0x88, 0x36, 0xd0, 0x94, 0xce, 0x8f, 0x96, 0xdb, 0xbd, 0xf1, 0xd2, 0x13, 0x5c, 0x83, 0x38, 0x46,
    0x40, 0x1e, 0x42, 0xb6, 0xa3, 0xc3, 0x48, 0x7e, 0x6e, 0x6b, 0x3a, 0x28, 0x54, 0xfa, 0x85, 0xba, 0x3d, 0xca, 0x5e,
    0x9b, 0x9f, 0x0a, 0x15, 0x79, 0x2b, 0x4e, 0xd4, 0xe5, 0xac, 0x73, 0xf3, 0xa7, 0x57, 0x07, 0x70, 0xc0, 0xf7, 0x8c,
    0x80, 0x63, 0x0d, 0x67, 0x4a, 0xde, 0xed, 0x31, 0xc5, 0xfe, 0x18, 0xe3, 0xa5, 0x99, 0x77, 0x26, 0xb8, 0xb4, 0x7c,
    0x11, 0x44, 0x92, 0xd9, 0x23, 0x20, 0x89, 0x2e, 0x37, 0x3f, 0xd1, 0x5b, 0x95, 0xbc, 0xcf, 0xcd, 0x90, 0x87, 0x97,
    0xb2, 0xdc, 0xfc, 0xbe, 0x61, 0xf2, 0x56, 0xd3, 0xab, 0x14, 0x2a, 0x5d, 0x9e, 0x84, 0x3c, 0x39, 0x53, 0x47, 0x6d,
    0x41, 0xa2, 0x1f, 0x2d, 0x43, 0xd8, 0xb7, 0x7b, 0xa4, 0x76, 0xc4, 0x17, 0x49, 0xec, 0x7f, 0x0c, 0x6f, 0xf6, 0x6c,
    0xa1, 0x3b, 0x52, 0x29, 0x9d, 0x55, 0xaa, 0xfb, 0x60, 0x86, 0xb1, 0xbb, 0xcc, 0x3e, 0x5a, 0xcb, 0x59, 0x5f, 0xb0,
    0x9c, 0xa9, 0xa0, 0x51, 0x0b, 0xf5, 0x16, 0xeb, 0x7a, 0x75, 0x2c, 0xd7, 0x4f, 0xae, 0xd5, 0xe9, 0xe6, 0xe7, 0xad,
    0xe8, 0x74, 0xd6, 0xf4, 0xea, 0xa8, 0x50, 0x58, 0xaf,
};

/* ---- Small helpers ---- */

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    unsigned s = (unsigned)gf_log[a] + gf_log[b];
    return gf_exp[s >= 255 ? s - 255 : s];
}

/* Codewords (data + ECC) of a version: modules left after the function
 * patterns, divided by 8 (remainder bits stay light). */
static int raw_codewords(int ver) {
    int bits = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int n = ver / 7 + 2;
        bits -= (25 * n - 10) * n - 55;
        if (ver >= 7) bits -= 36;
    }
    return bits / 8;
}

static int data_codewords(int ver) {
    return raw_codewords(ver) - ecc_per_block[ver] * num_blocks[ver];
}

static int count_bits(int ver) {
    return ver < 10 ? 8 : 16;
}

size_t qr_capacity(int ver) {
    if (ver < 1 || ver > QR_VERSION_MAX) return 0;
    return (size_t)(data_codewords(ver) * 8 - 4 - count_bits(ver)) / 8;
}

static int bit_get(const uint8_t *map, int x, int y) {
    return (map[y * QR_ROW_BYTES + x / 8] >> (7 - x % 8)) & 1;
}

static void bit_put(uint8_t *map, int x, int y, int v) {
    uint8_t m = (uint8_t)(0x80 >> (x % 8));
    if (v)
        map[y * QR_ROW_BYTES + x / 8] |= m;
    else
        map[y * QR_ROW_BYTES + x / 8] &= (uint8_t)~m;
}

static void set_function(QrCode *qr, int x, int y, int dark) {
    bit_put(qr->modules, x, y, dark);
    bit_put(qr->function, x, y, 1);
}

/* ---- Function patterns ---- */

/* Finder (7x7 rings) plus its light separator, centred on (cx, cy). */
static void draw_finder(QrCode *qr, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= qr->size || y >= qr->size) continue;
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int d = ax > ay ? ax : ay;
            set_function(qr, x, y, d != 2 && d != 4);
        }
    }
}

static void draw_alignment(QrCode *qr, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            set_function(qr, cx + dx, cy + dy, (ax > ay ? ax : ay) != 1);
        }
    }
}

/* Centre coordinates of the alignment patterns (same list for x and y):
 * 6, then evenly spaced (even steps) up to size - 7. Returns the count. */
static int alignment_positions(int ver, int pos[7]) {
    if (ver == 1) return 0;
    int n = ver / 7 + 2;
    int step = (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    pos[0] = 6;
    for (int i = n - 1, p = ver * 4 + 10; i >= 1; i--, p -= step) {
        pos[i] = p;
    }
    return n;
}

/* Error-correction level bits of the format word (L 01, M 00, Q 11, H 10) */
#define QR_ECL_M 0u

/* 15-bit format word: level bits and the mask, BCH(15,5) with generator
 * 0x537, XORed with 0x5412 so it is never all zero. */
static uint32_t format_bits(uint32_t ecl, int mask) {
    uint32_t data = (ecl << 3) | (uint32_t)mask;
    uint32_t rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return ((data << 10) | rem) ^ 0x5412;
}

/* 18-bit version word (versions 7+): BCH(18,6) with generator 0x1F25. */
static uint32_t version_bits(int ver) {
    uint32_t rem = (uint32_t)ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    return ((uint32_t)ver << 12) | rem;
}

/* Both copies of the format word, plus the dark module beside the bottom-left finder. */
static void draw_format(QrCode *qr, int mask) {
    uint32_t bits = format_bits(QR_ECL_M, mask);
    int n = qr->size;
    for (int i = 0; i <= 5; i++) set_function(qr, 8, i, (bits >> i) & 1);
    set_function(qr, 8, 7, (bits >> 6) & 1);
    set_function(qr, 8, 8, (bits >> 7) & 1);
    set_function(qr, 7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) set_function(qr, 14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; i++) set_function(qr, n - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) set_function(qr, 8, n - 15 + i, (bits >> i) & 1);
    set_function(qr, 8, n - 8, 1);
}

static void draw_version(QrCode *qr) {
    if (qr->version < 7) return;
    uint32_t bits = version_bits(qr->version);
    for (int i = 0; i < 18; i++) {
        int a = qr->size - 11 + i % 3, b = i / 3;
        int dark = (bits >> i) & 1;
        set_function(qr, a, b, dark);
        set_function(qr, b, a, dark);
    }
}

static void draw_function_patterns(QrCode *qr) {
    int n = qr->size;
    for (int i = 0; i < n; i++) {
        set_function(qr, 6, i, i % 2 == 0);
        set_function(qr, i, 6, i % 2 == 0);
    }
    draw_finder(qr, 3, 3);
    draw_finder(qr, n - 4, 3);
    draw_finder(qr, 3, n - 4);

    int pos[7];
    int cnt = alignment_positions(qr->version, pos);
    for (int i = 0; i < cnt; i++) {
        for (int j = 0; j < cnt; j++) {
            /* The three corners already hold finders */
            if ((i == 0 && j == 0) || (i == 0 && j == cnt - 1) || (i == cnt - 1 && j == 0)) continue;
            draw_alignment(qr, pos[i], pos[j]);
        }
    }
    draw_format(qr, 0);   /* placeholder: marks the format area as function modules */
    draw_version(qr);
}

/* ---- Codewords ---- */

static void put_bits(uint8_t *buf, int *bitpos, uint32_t val, int nbits) {
    for (int i = nbits - 1; i >= 0; i--, (*bitpos)++) {
        if ((val >> i) & 1) buf[*bitpos >> 3] |= (uint8_t)(0x80 >> (*bitpos & 7));
    }
}

/* Data codewords: mode, count, bytes, terminator, zero bits to a byte, pad bytes. */
static void encode_data(QrCode *qr, const uint8_t *data, size_t len) {
    int ndata = data_codewords(qr->version);
    int bit = 0;
    memset(qr->codewords, 0, sizeof(qr->codewords));
    put_bits(qr->codewords, &bit, 0x4, 4);
    put_bits(qr->codewords, &bit, (uint32_t)len, count_bits(qr->version));
    for (size_t i = 0; i < len; i++) put_bits(qr->codewords, &bit, data[i], 8);
    int room = ndata * 8 - bit;
    bit += room < 4 ? room : 4;           /* terminator: up to four 0 bits */
    bit = (bit + 7) & ~7;                 /* zero bits to the byte boundary */
    for (uint8_t pad = 0xEC; bit < ndata * 8; pad ^= 0xEC ^ 0x11) put_bits(qr->codewords, &bit, pad, 8);
}

/* Reed-Solomon generator polynomial of the given degree, (x - 2^0)...(x - 2^(d-1)),
 * highest coefficient (always 1) left out, coefficients from x^(d-1) down. */
static void rs_divisor(int degree, uint8_t *div) {
    memset(div, 0, (size_t)degree);
    div[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            div[j] = gf_mul(div[j], root);
            if (j + 1 < degree) div[j] ^= div[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

/* ECC codewords of one block: remainder of data(x) * x^degree divided by the generator. */
static void rs_remainder(const uint8_t *data, int len, const uint8_t *div, int degree, uint8_t *out) {
    memset(out, 0, (size_t)degree);
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)degree - 1);
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++) out[j] ^= gf_mul(div[j], factor);
    }
}

/* Block layout: the first `nshort` blocks hold `short_len` data codewords,
 * the rest one more. Codewords are stored block after block, data first:
 * codewords[0..ndata) = data blocks, then each block's ECC in turn. */
typedef struct {
    int nblocks, ecc, ndata, nshort, short_len;
} BlockLayout;

static BlockLayout block_layout(int ver) {
    BlockLayout b;
    b.nblocks = num_blocks[ver];
    b.ecc = ecc_per_block[ver];
    b.ndata = data_codewords(ver);
    int raw = raw_codewords(ver);
    b.nshort = b.nblocks - raw % b.nblocks;
    b.short_len = raw / b.nblocks - b.ecc;
    return b;
}

static int block_start(const BlockLayout *b, int blk) {
    return blk * b->short_len + (blk > b->nshort ? blk - b->nshort : 0);
}

static void add_ecc(QrCode *qr) {
    BlockLayout b = block_layout(qr->version);
    uint8_t div[30];   /* 30 = largest ecc_per_block entry */
    rs_divisor(b.ecc, div);
    for (int blk = 0; blk < b.nblocks; blk++) {
        int len = b.short_len + (blk >= b.nshort);
        rs_remainder(qr->codewords + block_start(&b, blk), len, div, b.ecc, qr->codewords + b.ndata + blk * b.ecc);
    }
}

/* Codeword k of the interleaved stream: data codeword i of every block in
 * turn (the short blocks have no last one), then ECC codeword i of every block. */
static uint8_t interleaved(const QrCode *qr, const BlockLayout *b, int k) {
    if (k >= b->ndata) {
        k -= b->ndata;
        return qr->codewords[b->ndata + (k % b->nblocks) * b->ecc + k / b->nblocks];
    }
    int full = b->nblocks * b->short_len;
    if (k < full) return qr->codewords[block_start(b, k % b->nblocks) + k / b->nblocks];
    return qr->codewords[block_start(b, b->nshort + (k - full)) + b->short_len];
}

/* Zigzag fill: column pairs from the right, upwards then downwards, skipping
 * the vertical timing line at x = 6 and every function module. */
static void place_data(QrCode *qr) {
    BlockLayout b = block_layout(qr->version);
    int total = raw_codewords(qr->version) * 8;
    int i = 0;
    uint8_t cur = 0;
    for (int right = qr->size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        int upward = ((right + 1) & 2) == 0;
        for (int vert = 0; vert < qr->size; vert++) {
            int y = upward ? qr->size - 1 - vert : vert;
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                if (bit_get(qr->function, x, y)) continue;
                int dark = 0;           /* remainder bits past the last codeword stay light */
                if (i < total) {
                    if ((i & 7) == 0) cur = interleaved(qr, &b, i >> 3);
                    dark = (cur >> (7 - (i & 7))) & 1;
                    i++;
                }
                bit_put(qr->modules, x, y, dark);
            }
        }
    }
}

/* ---- Masks and penalty ---- */

/* Mask condition at row y, column x (ISO/IEC 18004 table 10: i = row, j = column). */
static int mask_bit(int mask, int x, int y) {
    switch (mask) {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return x * y % 2 + x * y % 3 == 0;
    case 6:
        return (x * y % 2 + x * y % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

/* XOR the mask into every data module; applying it twice undoes it. */
static void apply_mask(QrCode *qr, int mask) {
    for (int y = 0; y < qr->size; y++) {
        for (int x = 0; x < qr->size; x++) {
            if (!bit_get(qr->function, x, y) && mask_bit(mask, x, y)) {
                qr->modules[y * QR_ROW_BYTES + x / 8] ^= (uint8_t)(0x80 >> (x % 8));
            }
        }
    }
}

#define PENALTY_N1 3
#define PENALTY_N2 3
#define PENALTY_N3 40
#define PENALTY_N4 10

/* Rules 1 and 3 along one row (horiz) or column, index `line`.
 * Rule 1: a run of 5 + k modules of one colour costs N1 + k.
 * Rule 3: 1:1:3:1:1 dark-light pattern (1011101) with 4 light modules on one
 * side costs N3 per side; modules outside the symbol count as light (they are
 * the quiet zone). An 11-module window slides along the line. */
static int penalty_line(const QrCode *qr, int horiz, int line) {
    int n = qr->size, score = 0, run = 0, prev = -1;
    uint32_t win = 0;
    for (int p = 0; p < n + 4; p++) {
        int dark = 0;
        if (p < n) {
            dark = horiz ? bit_get(qr->modules, p, line) : bit_get(qr->modules, line, p);
            if (dark == prev) {
                run++;
            } else {
                if (run >= 5) score += PENALTY_N1 + run - 5;
                run = 1;
                prev = dark;
            }
        }
        win = ((win << 1) | (uint32_t)dark) & 0x7FF;
        if (win == 0x5D0 || win == 0x05D) score += PENALTY_N3;
    }
    if (run >= 5) score += PENALTY_N1 + run - 5;
    return score;
}

/* Rule 2: every 2x2 block of one colour costs N2. */
static int penalty_blocks(const QrCode *qr) {
    int score = 0;
    for (int y = 0; y + 1 < qr->size; y++) {
        for (int x = 0; x + 1 < qr->size; x++) {
            int c = bit_get(qr->modules, x, y);
            if (c == bit_get(qr->modules, x + 1, y) && c == bit_get(qr->modules, x, y + 1) &&
                c == bit_get(qr->modules, x + 1, y + 1)) {
                score += PENALTY_N2;
            }
        }
    }
    return score;
}

/* Rule 4: N4 for every full 5% the dark share is away from 50%. */
static int penalty_balance(const QrCode *qr) {
    int dark = 0, total = qr->size * qr->size;
    for (int y = 0; y < qr->size; y++) {
        for (int x = 0; x < qr->size; x++) dark += bit_get(qr->modules, x, y);
    }
    int diff = dark * 20 - total * 10;
    if (diff < 0) diff = -diff;
    int k = (diff + total - 1) / total - 1;   /* total is odd, so diff > 0 and k >= 0 */
    return (k > 0 ? k : 0) * PENALTY_N4;
}

static int penalty(const QrCode *qr) {
    int score = penalty_blocks(qr) + penalty_balance(qr);
    for (int i = 0; i < qr->size; i++) {
        score += penalty_line(qr, 1, i) + penalty_line(qr, 0, i);
    }
    return score;
}

/* ---- Entry point ---- */

int qr_encode(const uint8_t *data, size_t len, int max_version, QrCode *out) {
    if (out == NULL || max_version < 1 || max_version > QR_VERSION_MAX || (data == NULL && len > 0)) {
        return -EINVAL;
    }
    int ver = 1;
    while (qr_capacity(ver) < len) {
        if (++ver > max_version) return -E2BIG;
    }

    out->version = ver;
    out->size = 17 + 4 * ver;
    memset(out->modules, 0, sizeof(out->modules));
    memset(out->function, 0, sizeof(out->function));
    draw_function_patterns(out);
    encode_data(out, data, len);
    add_ecc(out);
    place_data(out);

    /* Try every mask with its format word in place; keep the lowest score */
    int best = 0, best_score = 0;
    for (int m = 0; m < 8; m++) {
        apply_mask(out, m);
        draw_format(out, m);
        int s = penalty(out);
        if (m == 0 || s < best_score) {
            best = m;
            best_score = s;
        }
        apply_mask(out, m);
    }
    apply_mask(out, best);
    draw_format(out, best);
    out->mask = best;
    return ver;
}
