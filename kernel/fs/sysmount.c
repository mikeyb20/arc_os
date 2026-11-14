/* arc_os — mounting arc_os's own partitions at boot (M1-13): see sysmount.h
 *
 * Owns: nothing after it returns (the superblocks belong to their mounts).
 * Invariant: only partitions with arc_os's type GUIDs are ever passed to
 * ext2_mount; labels and the command line only choose among those.
 * Locks: none of its own; runs once from kmain's devices_setup, before init,
 * with the block registry complete (blk_lookup.c reads it lock-free). */

#include "fs/sysmount.h"

#include "arc_disk.h"
#include "drivers/blkdev.h"
#include "drivers/gpt.h"
#include "fs/ext2.h"
#include "fs/vfs.h"
#include "lib/cmdline.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "uapi/errno.h"

/* The data partition's directories and where each is mounted (ADR-0009) */
static const char *const data_dirs[] = {"apps", "models", "packs", "home", "var"};

/* The arc.data=PARTUUID=<guid> partition: 1 (*out set, or NULL when it is
 * refused), or 0 when the command line doesn't name one */
static int named_data(const char *cmdline, const Guid *data_type, BlockDevice **out) {
    char val[64];
    *out = NULL;
    if (cmdline == NULL || !cmdline_get(cmdline, "arc.data", val, sizeof(val))) return 0;
    Guid g;
    if (strncmp(val, "PARTUUID=", 9) != 0 || gpt_guid_parse(val + 9, &g) != 0) {
        kprintf("[MOUNT] arc.data=%s: expected PARTUUID=<guid>; no data partition\n", val);
        return 1;
    }
    BlockDevice *d = blk_find_partuuid(&g);
    if (d == NULL) {
        kprintf("[MOUNT] arc.data: no partition %s; no data partition\n", val + 9);
    } else if (!gpt_guid_equal(&d->type_guid, data_type)) {
        kprintf("[MOUNT] arc.data: %s is not an arc_os data partition; not mounted\n", d->name);
    } else {
        *out = d;
    }
    return 1;
}

void sysmount_pick(const uint8_t *boot_disk_guid, const char *cmdline, SysmountPick *out) {
    Guid sys_type, data_type;
    gpt_guid_parse(ARC_PART_TYPE_SYSTEM, &sys_type);
    gpt_guid_parse(ARC_PART_TYPE_DATA, &data_type);
    out->system = out->data = NULL;
    const Guid *disk = NULL;
    Guid boot;
    char text[GUID_STR_LEN];
    if (boot_disk_guid != NULL) {
        memcpy(boot.b, boot_disk_guid, sizeof(boot.b));
        gpt_guid_format(&boot, text);
        BlockDevice *d = blk_find_disk(&boot);
        BlockDevice *sys = d != NULL ? blk_find_part(&boot, &sys_type) : NULL;
        if (d != NULL && (sys != NULL || blk_find_part(&boot, &data_type) != NULL)) {
            kprintf("[MOUNT] boot disk %s is %s\n", text, d->name);
            disk = &boot;
            out->system = sys;
        } else if (d != NULL) {   /* an ISO image written to a stick: no arc_os partitions on it */
            kprintf("[MOUNT] boot disk %s (%s) has no arc_os partitions: by label\n", text, d->name);
        } else {
            /* An ISO image, or a disk no driver serves yet (RAM mode) */
            kprintf("[MOUNT] boot disk %s not among the disks: by label\n", text);
        }
    }
    if (disk == NULL) {
        /* Boot disk unknown: the label picks the disk, the type must match */
        out->system = blk_find_label(ARC_PART_LABEL_SYSTEM, &sys_type);
        if (out->system != NULL) {
            disk = &out->system->disk_guid;
            kprintf("[MOUNT] %s by label \"%s\"\n", out->system->disk->name, ARC_PART_LABEL_SYSTEM);
        } else if ((out->data = blk_find_label(ARC_PART_LABEL_DATA, &data_type)) != NULL) {
            disk = &out->data->disk_guid;
        } else {
            kprintf("[MOUNT] no arc_os partition labelled \"%s\" or \"%s\"\n", ARC_PART_LABEL_SYSTEM,
                    ARC_PART_LABEL_DATA);
        }
    }
    BlockDevice *named;
    if (named_data(cmdline, &data_type, &named)) {
        out->data = named;
    } else if (out->data == NULL && disk != NULL) {
        out->data = blk_find_part(disk, &data_type);
    }
}

/* ext2 on dev: its superblock, or NULL (logged) */
static SuperBlock *mount_ext2(BlockDevice *dev, const char *what) {
    int err;
    SuperBlock *sb = ext2_mount(dev, dev->nr_sectors * 512, dev->name, &err);
    if (sb == NULL) kprintf("[MOUNT] %s partition %s: not mountable (%d)\n", what, dev->name, err);
    return sb;
}

static int mount_at(const char *target, SuperBlock *sb, const char *name, BlockDevice *dev) {
    int err = vfs_mkdir(target, 0755);
    if (err == -EEXIST) err = 0;
    if (err == 0) err = name != NULL ? vfs_mount_subdir(target, sb, name, dev->name) : vfs_mount(target, sb, dev->name);
    if (err != 0) {
        kprintf("[MOUNT] %s from %s: failed (%d)\n", target, dev->name, err);
        return 0;
    }
    kprintf("[MOUNT] %s: %s%s%s (ext2, read-only)\n", target, dev->name, name != NULL ? " /" : "",
            name != NULL ? name : "");
    return 1;
}

int sysmount_boot(const uint8_t *boot_disk_guid, const char *cmdline) {
    SysmountPick p;
    sysmount_pick(boot_disk_guid, cmdline, &p);
    int mounted = 0;
    SuperBlock *sb;
    if (p.system != NULL && (sb = mount_ext2(p.system, "system")) != NULL)
        mounted += mount_at("/system", sb, NULL, p.system);
    if (p.data != NULL && (sb = mount_ext2(p.data, "data")) != NULL) {
        for (unsigned i = 0; i < sizeof(data_dirs) / sizeof(data_dirs[0]); i++) {
            char target[16] = "/";
            strncpy(target + 1, data_dirs[i], sizeof(target) - 2);
            mounted += mount_at(target, sb, data_dirs[i], p.data);
        }
    }
    return mounted;
}
