/* arc_os — which partitions arc_os mounts at boot (M1-13, ADR-0018), on the
 * real sysmount.c and gpt.c over a made-up device list: the boot disk by
 * GUID, the label fallback that still needs arc_os's type GUIDs, and
 * arc.data=PARTUUID=. Foreign partitions are never picked, whatever their
 * labels say. (Mounting itself runs in qemu.ext2.) */

#include "test_framework.h"
#include "vfs_env.h"
#include "blk_mem.h"

#include "arc_disk.h"
#include "drivers/blkdev.h"
#include "drivers/gpt.h"
#include "fs/sysmount.h"

#define LINUX_TYPE   "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define WINDOWS_TYPE "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
#define GUID_A       "AAAAAAAA-0000-4000-8000-000000000000"
#define GUID_B       "BBBBBBBB-0000-4000-8000-000000000000"
#define GUID_C       "CCCCCCCC-0000-4000-8000-000000000000"
#define C2_UUID      "CCCCCCCC-0000-4000-8000-000000000002"
#define A2_UUID      "AAAAAAAA-0000-4000-8000-000000000002"

/* The block layer's device list (blk_mem_devices), as sysmount.c's lookups
 * walk it */
static BlockDevice devs[12];
static BlockDevice *list[12];
static unsigned ndevs;

static BlockDevice *new_dev(void) {
    BlockDevice *d = &devs[ndevs];
    list[ndevs++] = d;
    blk_mem_devices(list, ndevs);
    memset(d, 0, sizeof(*d));
    return d;
}

static void no_devs(void) {
    ndevs = 0;
    blk_mem_devices(list, 0);
}

static BlockDevice *add_disk(const char *name, const char *guid) {
    BlockDevice *d = new_dev();
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->disk = d;
    d->has_gpt = 1;
    gpt_guid_parse(guid, &d->disk_guid);
    return d;
}

static BlockDevice *add_part(BlockDevice *disk, unsigned n, const char *type, const char *label, const char *uuid) {
    BlockDevice *d = new_dev();
    snprintf(d->name, sizeof(d->name), "%.8s%u", disk->name, n);
    d->disk = disk;
    d->partno = n;
    d->has_gpt = 1;
    d->disk_guid = disk->disk_guid;
    gpt_guid_parse(type, &d->type_guid);
    if (uuid != NULL) gpt_guid_parse(uuid, &d->part_guid);
    snprintf(d->label, sizeof(d->label), "%s", label);
    return d;
}

/* vda: foreign, labelled like arc_os; vdb: arc_os; vdc: arc_os, other labels */
static BlockDevice *a1, *a2, *b1, *b2, *c1, *c2;

static void setup(void) {
    vfs_env_setup();
    no_devs();
    BlockDevice *a = add_disk("vda", GUID_A);
    a1 = add_part(a, 1, LINUX_TYPE, ARC_PART_LABEL_SYSTEM, NULL);
    a2 = add_part(a, 2, WINDOWS_TYPE, ARC_PART_LABEL_DATA, A2_UUID);
    BlockDevice *b = add_disk("vdb", GUID_B);
    b1 = add_part(b, 1, ARC_PART_TYPE_SYSTEM, ARC_PART_LABEL_SYSTEM, NULL);
    b2 = add_part(b, 2, ARC_PART_TYPE_DATA, ARC_PART_LABEL_DATA, NULL);
    BlockDevice *c = add_disk("vdc", GUID_C);
    c1 = add_part(c, 1, ARC_PART_TYPE_SYSTEM, "mine", NULL);
    c2 = add_part(c, 2, ARC_PART_TYPE_DATA, "also mine", C2_UUID);
}

static void pick(const char *boot, const char *cmdline, SysmountPick *p) {
    Guid g;
    if (boot != NULL) gpt_guid_parse(boot, &g);
    sysmount_pick(boot != NULL ? g.b : NULL, cmdline, p);
}

TEST(boot_disk_by_guid) {
    setup();
    SysmountPick p;
    pick(GUID_C, "", &p);   /* labels play no part */
    ASSERT_TRUE(p.system == c1);
    ASSERT_TRUE(p.data == c2);
    ASSERT_TRUE(p.disk == c1->disk);
    pick(GUID_B, NULL, &p);
    ASSERT_TRUE(p.system == b1);
    ASSERT_TRUE(p.data == b2);
    return 0;
}

/* Found, but nothing on it has arc_os's types (the bring-up ISO written to
 * a stick): the labels pick the disk, never the foreign partitions */
TEST(boot_disk_without_arc_partitions_uses_labels) {
    setup();
    SysmountPick p;
    pick(GUID_A, "", &p);
    ASSERT_TRUE(p.system == b1);
    ASSERT_TRUE(p.data == b2);
    ASSERT_TRUE(p.disk == b1->disk);   /* /disk is vdb's ESP, never vda's */
    return 0;
}

TEST(label_fallback_needs_the_type) {
    setup();
    SysmountPick p;
    pick(NULL, "", &p);   /* vda's labels come first but its types are foreign */
    ASSERT_TRUE(p.system == b1);
    ASSERT_TRUE(p.data == b2);
    pick("12345678-0000-4000-8000-000000000000", "", &p);   /* an ISO's GUID: no such disk */
    ASSERT_TRUE(p.system == b1);
    ASSERT_TRUE(p.data == b2);
    /* Only the data partition labelled */
    no_devs();
    BlockDevice *d = add_disk("vda", GUID_A);
    BlockDevice *d1 = add_part(d, 1, ARC_PART_TYPE_DATA, ARC_PART_LABEL_DATA, NULL);
    pick(NULL, "", &p);
    ASSERT_TRUE(p.system == NULL);
    ASSERT_TRUE(p.data == d1);
    /* No arc_os partition anywhere */
    no_devs();
    d = add_disk("vda", GUID_A);
    add_part(d, 1, LINUX_TYPE, ARC_PART_LABEL_SYSTEM, NULL);
    add_part(d, 2, WINDOWS_TYPE, ARC_PART_LABEL_DATA, NULL);
    pick(NULL, "", &p);
    ASSERT_TRUE(p.system == NULL);
    ASSERT_TRUE(p.data == NULL);
    ASSERT_TRUE(p.disk == NULL);   /* and nothing at /disk */
    return 0;
}

/* A cloned disk (dd) has the same GPT GUID: both partitions come from the
 * first such disk, never one from each */
TEST(cloned_disk_guid_stays_on_one_disk) {
    setup();
    no_devs();
    BlockDevice *orig = add_disk("vda", GUID_B);
    BlockDevice *o1 = add_part(orig, 1, ARC_PART_TYPE_SYSTEM, ARC_PART_LABEL_SYSTEM, NULL);
    BlockDevice *clone = add_disk("vdb", GUID_B);
    add_part(clone, 1, ARC_PART_TYPE_SYSTEM, ARC_PART_LABEL_SYSTEM, NULL);
    add_part(clone, 2, ARC_PART_TYPE_DATA, ARC_PART_LABEL_DATA, NULL);
    SysmountPick p;
    pick(GUID_B, "", &p);
    ASSERT_TRUE(p.disk == orig);
    ASSERT_TRUE(p.system == o1);
    ASSERT_TRUE(p.data == NULL);   /* not the clone's */
    pick(NULL, "", &p);            /* by label: the same */
    ASSERT_TRUE(p.system == o1);
    ASSERT_TRUE(p.data == NULL);
    return 0;
}

TEST(data_named_by_partuuid) {
    setup();
    SysmountPick p;
    pick(GUID_B, "quiet arc.data=PARTUUID=" C2_UUID, &p);
    ASSERT_TRUE(p.system == b1);
    ASSERT_TRUE(p.data == c2);
    pick(GUID_B, "arc.data=PARTUUID=cccccccc-0000-4000-8000-000000000002", &p);   /* either case */
    ASSERT_TRUE(p.data == c2);
    /* A foreign partition named: refused, and no fallback to the boot disk's */
    pick(GUID_B, "arc.data=PARTUUID=" A2_UUID, &p);
    ASSERT_TRUE(p.system == b1);
    ASSERT_TRUE(p.data == NULL);
    /* Not there, or not a GUID: no data either */
    pick(GUID_B, "arc.data=PARTUUID=DDDDDDDD-0000-4000-8000-000000000000", &p);
    ASSERT_TRUE(p.data == NULL);
    pick(GUID_B, "arc.data=LABEL=arc-data", &p);
    ASSERT_TRUE(p.data == NULL);
    pick(GUID_B, "arc.data=PARTUUID=not-a-guid", &p);
    ASSERT_TRUE(p.data == NULL);
    return 0;
}

TestCase sysmount_tests[] = {
    TEST_ENTRY(boot_disk_by_guid),
    TEST_ENTRY(boot_disk_without_arc_partitions_uses_labels),
    TEST_ENTRY(label_fallback_needs_the_type),
    TEST_ENTRY(cloned_disk_guid_stays_on_one_disk),
    TEST_ENTRY(data_named_by_partuuid),
};
int sysmount_test_count = sizeof(sysmount_tests) / sizeof(sysmount_tests[0]);
