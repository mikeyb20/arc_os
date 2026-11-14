/* arc_os — Host-side tests for kernel/drivers/gpt.c (M1-10)
 *
 * The real parser reads image files made by tools/make-gpt-test-disk.sh
 * (fixture gpt_images): an sgdisk disk, the same with a broken primary header
 * or primary entry array (the backup must be used), a 4Kn image, and the
 * FAT32 test image without any GPT. */

#include "test_framework.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARCHOS_LIB_MEM_H   /* libc memcmp/memcpy/memset */
#include "../kernel/drivers/gpt.c"
#include "arc_disk.h"

#define GPT_DIR "tests/gpt/"

typedef struct {
    FILE *f;
    uint32_t lbs;
    int reads;
} ImgCtx;

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    ImgCtx *c = ctx;
    c->reads++;
    if (fseek(c->f, (long)(lba * c->lbs), SEEK_SET) != 0) return -EIO;
    return fread(buf, c->lbs, count, c->f) == count ? 0 : -EIO;
}

/* Parse path with blocks of lbs bytes; returns gpt_read's result */
static int parse(const char *path, uint32_t lbs, GptDisk *d, GptPart *parts, uint32_t max) {
    static uint8_t scratch[2 * 4096];
    ImgCtx c = {fopen(path, "rb"), lbs, 0};
    if (c.f == NULL) {
        printf("    (missing %s: run the gpt_images fixture)\n", path);
        return -999;
    }
    fseek(c.f, 0, SEEK_END);
    uint64_t blocks = (uint64_t)ftell(c.f) / lbs;
    int rc = gpt_read(img_read, &c, lbs, blocks, scratch, d, parts, max);
    fclose(c.f);
    return rc;
}

static int guid_is(const Guid *g, const char *text) {
    Guid want;
    return gpt_guid_parse(text, &want) == 0 && gpt_guid_equal(g, &want);
}

/* The layout tools/make-gpt-test-disk.sh writes, in blocks of lbs bytes */
static int check_layout(const GptDisk *d, const GptPart *p, uint32_t lbs) {
    uint64_t k = lbs == 512 ? 1 : 8;   /* the 4Kn image uses the same bytes */
    ASSERT_EQ(d->nparts, 3);
    ASSERT_TRUE(guid_is(&d->disk_guid, "4A7D3F10-1B2C-4D5E-8F90-A1B2C3D4E5F6"));
    ASSERT_TRUE(guid_is(&p[0].type, ARC_PART_TYPE_SYSTEM));
    ASSERT_TRUE(guid_is(&p[0].guid, "11111111-2222-4333-8444-555555555551"));
    ASSERT_STR_EQ(p[0].label, ARC_PART_LABEL_SYSTEM);
    ASSERT_EQ(p[0].first_lba, 2048 / k);
    ASSERT_EQ(p[0].last_lba, 4096 / k - 1);
    ASSERT_EQ(p[0].number, 1);
    ASSERT_TRUE(guid_is(&p[1].type, ARC_PART_TYPE_DATA));
    ASSERT_STR_EQ(p[1].label, ARC_PART_LABEL_DATA);
    ASSERT_EQ(p[1].first_lba, 4096 / k);
    ASSERT_EQ(p[2].number, 3);
    ASSERT_STR_EQ(p[2].label, "donn\xc3\xa9"
                              "es-\xe2\x9c\x93");   /* UTF-16 to UTF-8 */
    ASSERT_EQ(p[2].last_lba, 10240 / k - 1);
    return 0;
}

TEST(gpt_guid_text) {
    Guid g;
    char out[GUID_STR_LEN];
    ASSERT_EQ(gpt_guid_parse("c12a7328-f81f-11d2-ba4b-00a0c93ec93b", &g), 0);
    ASSERT_EQ(g.b[0], 0x28);   /* first field little-endian */
    ASSERT_EQ(g.b[3], 0xC1);
    ASSERT_EQ(g.b[8], 0xBA);   /* last two fields as written */
    gpt_guid_format(&g, out);
    ASSERT_STR_EQ(out, ARC_PART_TYPE_ESP);
    ASSERT_EQ(gpt_guid_parse("C12A7328-F81F-11D2-BA4B-00A0C93EC93", &g), -1);
    ASSERT_EQ(gpt_guid_parse("C12A7328-F81F-11D2-BA4B-00A0C93EC93BX", &g), -1);
    ASSERT_EQ(gpt_guid_parse("C12A7328F81F-11D2-BA4B-00A0C93EC93B0", &g), -1);
    ASSERT_EQ(gpt_guid_parse("G12A7328-F81F-11D2-BA4B-00A0C93EC93B", &g), -1);
    return 0;
}

TEST(gpt_sgdisk_image) {
    GptDisk d;
    GptPart p[8];
    ASSERT_EQ(parse(GPT_DIR "gpt512.img", 512, &d, p, 8), 0);
    ASSERT_EQ(d.backup_used, 0);
    ASSERT_EQ(check_layout(&d, p, 512), 0);
    return 0;
}

TEST(gpt_bad_primary_header_uses_backup) {
    GptDisk d;
    GptPart p[8];
    ASSERT_EQ(parse(GPT_DIR "gpt512-badhdr.img", 512, &d, p, 8), 0);
    ASSERT_EQ(d.backup_used, 1);
    ASSERT_EQ(check_layout(&d, p, 512), 0);
    return 0;
}

TEST(gpt_bad_primary_entries_use_backup) {
    GptDisk d;
    GptPart p[8];
    ASSERT_EQ(parse(GPT_DIR "gpt512-badents.img", 512, &d, p, 8), 0);
    ASSERT_EQ(d.backup_used, 1);
    ASSERT_EQ(check_layout(&d, p, 512), 0);
    return 0;
}

TEST(gpt_4kn_image) {
    GptDisk d;
    GptPart p[8];
    ASSERT_EQ(parse(GPT_DIR "gpt4k.img", 4096, &d, p, 8), 0);
    ASSERT_EQ(d.backup_used, 0);
    ASSERT_EQ(check_layout(&d, p, 4096), 0);
    /* Read as 512-byte blocks, the 4Kn disk has no GPT at LBA 1 or at the end */
    ASSERT_EQ(parse(GPT_DIR "gpt4k.img", 512, &d, p, 8), -EINVAL);
    return 0;
}

TEST(gpt_no_table) {
    GptDisk d;
    GptPart p[8];
    ASSERT_EQ(parse("tests/fat32_test.img", 512, &d, p, 8), -ENOENT);
    ASSERT_EQ(d.nparts, 0);
    return 0;
}

TEST(gpt_max_parts_skips_the_rest) {
    GptDisk d;
    GptPart p[2];
    ASSERT_EQ(parse(GPT_DIR "gpt512.img", 512, &d, p, 2), 0);
    ASSERT_EQ(d.nparts, 2);
    ASSERT_EQ(d.skipped, 1);
    return 0;
}

/* Both headers bad: an in-memory copy with both CRCs broken */
static uint8_t mem_disk[16384 * 512];

static uint64_t mem_bad_lba = UINT64_MAX;   /* a bad sector: reads covering it fail */

static int mem_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if ((lba + count) * 512 > sizeof(mem_disk)) return -EIO;
    if (mem_bad_lba >= lba && mem_bad_lba < lba + count) return -EIO;
    memcpy(buf, mem_disk + lba * 512, (size_t)count * 512);
    return 0;
}

TEST(gpt_both_headers_bad) {
    uint8_t scratch[1024];
    GptDisk d;
    GptPart p[8];
    FILE *f = fopen(GPT_DIR "gpt512.img", "rb");
    ASSERT_TRUE(f != NULL);
    size_t n = fread(mem_disk, 1, sizeof(mem_disk), f);
    fclose(f);
    ASSERT_EQ(n, sizeof(mem_disk));
    ASSERT_EQ(gpt_read(mem_read, NULL, 512, 16384, scratch, &d, p, 8), 0);
    mem_disk[512 + 16] ^= 1;
    mem_disk[sizeof(mem_disk) - 512 + 16] ^= 1;
    ASSERT_EQ(gpt_read(mem_read, NULL, 512, 16384, scratch, &d, p, 8), -EINVAL);
    ASSERT_EQ(d.nparts, 0);
    /* A read error is reported as such, not as "no table" */
    ASSERT_EQ(gpt_read(mem_read, NULL, 512, 1u << 20, scratch, &d, p, 8), -EIO);
    return 0;
}

/* A read error at the primary header or its entries falls back to the backup */
TEST(gpt_primary_read_error_uses_backup) {
    uint8_t scratch[1024];
    GptDisk d;
    GptPart p[8];
    FILE *f = fopen(GPT_DIR "gpt512.img", "rb");
    ASSERT_TRUE(f != NULL);
    size_t n = fread(mem_disk, 1, sizeof(mem_disk), f);
    fclose(f);
    ASSERT_EQ(n, sizeof(mem_disk));
    for (uint64_t bad = 1; bad <= 2; bad++) {   /* header, then first entry block */
        mem_bad_lba = bad;
        int rc = gpt_read(mem_read, NULL, 512, 16384, scratch, &d, p, 8);
        mem_bad_lba = UINT64_MAX;
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(d.backup_used, 1);
        ASSERT_EQ(check_layout(&d, p, 512), 0);
    }
    mem_bad_lba = 0;   /* the MBR itself: the error, not "no table" */
    ASSERT_EQ(gpt_read(mem_read, NULL, 512, 16384, scratch, &d, p, 8), -EIO);
    mem_bad_lba = UINT64_MAX;
    return 0;
}

TestCase gpt_tests[] = {
    TEST_ENTRY(gpt_guid_text),
    TEST_ENTRY(gpt_sgdisk_image),
    TEST_ENTRY(gpt_bad_primary_header_uses_backup),
    TEST_ENTRY(gpt_bad_primary_entries_use_backup),
    TEST_ENTRY(gpt_4kn_image),
    TEST_ENTRY(gpt_no_table),
    TEST_ENTRY(gpt_max_parts_skips_the_rest),
    TEST_ENTRY(gpt_both_headers_bad),
    TEST_ENTRY(gpt_primary_read_error_uses_backup),
};

int gpt_test_count = sizeof(gpt_tests) / sizeof(gpt_tests[0]);
