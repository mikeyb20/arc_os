#ifndef ARCHOS_DRIVERS_GPT_H
#define ARCHOS_DRIVERS_GPT_H

/* GPT partition tables (M1-10, ADR-0008): protective MBR, primary header and
 * entries with CRC32 checks, backup header when the primary is bad. Pure
 * parsing over a read callback, so the host tests run it on sgdisk images;
 * the block layer (blkdev.c) turns the result into partition devices. */

#include <stdint.h>

/* A GUID as GPT stores it (first three fields little-endian) */
typedef struct {
    uint8_t b[16];
} Guid;

/* Text form "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" plus NUL */
#define GUID_STR_LEN 37

/* Parse the text form (either case) into GPT byte order. Returns 0, or -1 if
 * s is not a GUID. */
int gpt_guid_parse(const char *s, Guid *out);

/* Upper-case text form of g into out (GUID_STR_LEN bytes). */
void gpt_guid_format(const Guid *g, char *out);

/* 1 if a and b are the same GUID. */
int gpt_guid_equal(const Guid *a, const Guid *b);

/* Partition name as UTF-8: 36 UTF-16 units, at most 3 bytes each, plus NUL */
#define GPT_LABEL_MAX 112

typedef struct {
    Guid type;
    Guid guid;                 /* PARTUUID */
    uint64_t first_lba;        /* logical blocks, inclusive */
    uint64_t last_lba;
    uint64_t attrs;
    uint32_t number;           /* entry index + 1, as Linux numbers partitions */
    char label[GPT_LABEL_MAX]; /* PARTLABEL, UTF-8 */
} GptPart;

typedef struct {
    Guid disk_guid;
    int backup_used;   /* 1 when the primary header or entries were bad */
    uint32_t nparts;   /* entries with a type GUID, written to parts[] */
    uint32_t skipped;  /* entries outside the usable range, or past max_parts */
} GptDisk;

/* Read count logical blocks starting at lba into buf. Returns 0 or -errno. */
typedef int (*GptReadFn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/* Read the GPT of a disk of nr_lbas logical blocks of lbs bytes (512 or
 * 4096). scratch holds 2 * lbs bytes. Fills disk and up to max_parts parts.
 * Returns 0, -ENOENT when there is no protective MBR (no GPT), -EINVAL when
 * both headers are bad, or the read callback's error. Reads O(entries)
 * blocks, one at a time; allocates nothing. */
int gpt_read(GptReadFn read, void *ctx, uint32_t lbs, uint64_t nr_lbas, void *scratch, GptDisk *disk, GptPart *parts,
             uint32_t max_parts);

#endif /* ARCHOS_DRIVERS_GPT_H */
