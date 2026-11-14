/* arc_os — GPT partition table parser (M1-10, ADR-0008)
 *
 * Owns: nothing; a pure parser over a read callback (see gpt.h).
 * Invariants: a header is used only if its signature, size, own CRC, own LBA,
 * ranges and entry-array CRC all check out; the primary first, else the
 * backup at the last block. Entries are read one block at a time and their
 * CRC is chained over the blocks, so nothing is allocated.
 * Locks: none. Cost: one read per header block and per entry block. */

#include "drivers/gpt.h"
#include "lib/crc32.h"
#include "lib/mem.h"
#include "uapi/errno.h"

#define GPT_MAX_ENTRIES     1024
#define MBR_TYPE_PROTECTIVE 0xEE

typedef struct __attribute__((packed)) {
    char signature[8];   /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    Guid disk_guid;
    uint64_t entries_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc;
} GptHeader;

typedef struct __attribute__((packed)) {
    Guid type;
    Guid guid;
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} GptEntry;

_Static_assert(sizeof(GptHeader) == 92, "GPT header is 92 bytes");
_Static_assert(sizeof(GptEntry) == 128, "GPT entry is 128 bytes");

/* --- GUIDs --- */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Byte i of the text form lands at GPT byte order[i] (fields 1-3 swapped) */
static const uint8_t guid_order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};

int gpt_guid_parse(const char *s, Guid *out) {
    int n = 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return -1;
            continue;
        }
        int hi = hexval(s[i]);
        int lo = hi >= 0 ? hexval(s[i + 1]) : -1;
        if (hi < 0 || lo < 0) return -1;
        out->b[guid_order[n++]] = (uint8_t)(hi << 4 | lo);
        i++;
    }
    return s[36] == '\0' ? 0 : -1;
}

void gpt_guid_format(const Guid *g, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    int pos = 0;
    for (int n = 0; n < 16; n++) {
        if (n == 4 || n == 6 || n == 8 || n == 10) out[pos++] = '-';
        uint8_t v = g->b[guid_order[n]];
        out[pos++] = hex[v >> 4];
        out[pos++] = hex[v & 0xF];
    }
    out[pos] = '\0';
}

int gpt_guid_equal(const Guid *a, const Guid *b) {
    return memcmp(a->b, b->b, sizeof(a->b)) == 0;
}

static int guid_is_zero(const Guid *g) {
    for (int i = 0; i < 16; i++) {
        if (g->b[i] != 0) return 0;
    }
    return 1;
}

/* --- Names: UTF-16LE to UTF-8 (ADR-0014) --- */

static void label_from_utf16(const uint16_t *in, char *out) {
    int pos = 0;
    for (int i = 0; i < 36 && in[i] != 0; i++) {
        uint32_t c = in[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < 36 && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;   /* unpaired surrogate */
        }
        if (c < 0x80) {
            out[pos++] = (char)c;
        } else if (c < 0x800) {
            out[pos++] = (char)(0xC0 | c >> 6);
            out[pos++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[pos++] = (char)(0xE0 | c >> 12);
            out[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[pos++] = (char)(0xF0 | c >> 18);
            out[pos++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[pos] = '\0';
}

/* --- Headers --- */

static int has_protective_mbr(const uint8_t *mbr) {
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {
        if (mbr[446 + 16 * i + 4] == MBR_TYPE_PROTECTIVE) return 1;
    }
    return 0;
}

/* Header checks that need no other block (the block was read from lba). */
static int header_ok(GptHeader *h, uint32_t lbs, uint64_t nr_lbas, uint64_t lba) {
    if (memcmp(h->signature, "EFI PART", 8) != 0) return 0;
    if (h->header_size < sizeof(GptHeader) || h->header_size > lbs) return 0;
    uint32_t want = h->header_crc;
    h->header_crc = 0;
    uint32_t got = crc32(0, h, h->header_size);
    h->header_crc = want;
    if (got != want || h->my_lba != lba) return 0;
    if (h->first_usable > h->last_usable || h->last_usable >= nr_lbas) return 0;
    /* Entry size: a power of two from 128 to a block, so no entry straddles blocks */
    uint32_t es = h->entry_size;
    if (es < sizeof(GptEntry) || es > lbs || (es & (es - 1)) != 0) return 0;
    if (h->num_entries == 0 || h->num_entries > GPT_MAX_ENTRIES) return 0;
    uint64_t blocks = ((uint64_t)h->num_entries * es + lbs - 1) / lbs;
    return h->entries_lba >= 1 && h->entries_lba < nr_lbas && blocks <= nr_lbas - h->entries_lba;
}

/* Read the entry array of h block by block: collect partitions and check
 * the array CRC. Returns 0 if the CRC matches, -EINVAL if not, or a read
 * error. On any failure disk->nparts is left at 0. */
static int read_entries(GptReadFn read, void *ctx, uint32_t lbs, const GptHeader *h, uint8_t *blk, GptDisk *disk,
                        GptPart *parts, uint32_t max_parts) {
    uint64_t bytes = (uint64_t)h->num_entries * h->entry_size;
    uint32_t per_block = lbs / h->entry_size;
    uint32_t crc = 0, index = 0;
    disk->nparts = disk->skipped = 0;
    for (uint64_t b = 0; index < h->num_entries; b++) {
        int rc = read(ctx, h->entries_lba + b, 1, blk);
        if (rc != 0) return rc;
        uint64_t left = bytes - b * lbs;
        crc = crc32(crc, blk, left < lbs ? (size_t)left : lbs);
        for (uint32_t i = 0; i < per_block && index < h->num_entries; i++, index++) {
            const GptEntry *e = (const GptEntry *)(blk + (uint64_t)i * h->entry_size);
            if (guid_is_zero(&e->type)) continue;
            if (e->first_lba < h->first_usable || e->last_lba > h->last_usable || e->first_lba > e->last_lba ||
                disk->nparts >= max_parts) {
                disk->skipped++;
                continue;
            }
            GptPart *p = &parts[disk->nparts++];
            p->type = e->type;
            p->guid = e->guid;
            p->first_lba = e->first_lba;
            p->last_lba = e->last_lba;
            p->attrs = e->attrs;
            p->number = index + 1;
            uint16_t name[36];
            memcpy(name, e->name, sizeof(name));
            label_from_utf16(name, p->label);
        }
    }
    if (crc == h->entries_crc) return 0;
    disk->nparts = disk->skipped = 0;
    return -EINVAL;
}

/* Try the header at lba. Returns 0, -EINVAL if it is bad, or a read error. */
static int try_header(GptReadFn read, void *ctx, uint32_t lbs, uint64_t nr_lbas, uint64_t lba, uint8_t *scratch,
                      GptDisk *disk, GptPart *parts, uint32_t max_parts) {
    int rc = read(ctx, lba, 1, scratch);
    if (rc != 0) return rc;
    GptHeader *h = (GptHeader *)scratch;
    if (!header_ok(h, lbs, nr_lbas, lba)) return -EINVAL;
    rc = read_entries(read, ctx, lbs, h, scratch + lbs, disk, parts, max_parts);
    if (rc == 0) disk->disk_guid = h->disk_guid;
    return rc;
}

int gpt_read(GptReadFn read, void *ctx, uint32_t lbs, uint64_t nr_lbas, void *scratch, GptDisk *disk, GptPart *parts,
             uint32_t max_parts) {
    uint8_t *buf = scratch;
    memset(disk, 0, sizeof(*disk));
    if ((lbs != 512 && lbs != 4096) || nr_lbas < 3) return -ENOENT;
    int rc = read(ctx, 0, 1, buf);
    if (rc != 0) return rc;
    if (!has_protective_mbr(buf)) return -ENOENT;

    rc = try_header(read, ctx, lbs, nr_lbas, 1, buf, disk, parts, max_parts);
    if (rc != -EINVAL) return rc;
    rc = try_header(read, ctx, lbs, nr_lbas, nr_lbas - 1, buf, disk, parts, max_parts);
    if (rc == 0) disk->backup_used = 1;
    return rc;
}
