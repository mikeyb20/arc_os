#ifndef ARCHOS_FS_SYSMOUNT_H
#define ARCHOS_FS_SYSMOUNT_H

/* Mounting arc_os's own partitions at boot (M1-13, ADR-0008, ADR-0009,
 * ADR-0018): the system partition read-only at /system, and the data
 * partition's apps, models, packs, home and var directories at /apps,
 * /models, /packs, /home and /var (subtree mounts).
 *
 * Which partitions (only ever ones with arc_os's own GPT type GUIDs,
 * include/arc_disk.h; anything else is never mounted):
 *   - The boot disk is an arc_os disk (Limine reported its GPT GUID, a driver
 *     serves it, and it has an arc_os system or data partition): system and
 *     data are that disk's partitions of those types.
 *   - Otherwise (an ISO image, also one written to a USB stick: Limine
 *     reports the image's own GUID; PXE; a disk no driver serves yet, RAM
 *     mode): the first partition labelled
 *     `arc-system` with the system type, and the data partition of that same
 *     disk. Labels only choose the disk; the type must still match.
 *   - `arc.data=PARTUUID=<guid>` on the command line names the data partition
 *     on another arc_os disk; it must have the data type, or no data is mounted.
 * Mount-time only: O(devices). */

#include <stdint.h>

struct BlockDevice;

typedef struct {
    struct BlockDevice *system;   /* NULL: none found */
    struct BlockDevice *data;
} SysmountPick;

/* Choose the partitions (rules above) without mounting anything.
 * boot_disk_guid: 16 bytes in GPT byte order, or NULL when unknown. cmdline:
 * the kernel command line (may be NULL). Logs each choice and refusal. */
void sysmount_pick(const uint8_t *boot_disk_guid, const char *cmdline, SysmountPick *out);

/* Pick and mount (ext2, read-only until M1-15). Returns how many of the six
 * mounts succeeded. A failure is logged and the rest go on. */
int sysmount_boot(const uint8_t *boot_disk_guid, const char *cmdline);

#endif /* ARCHOS_FS_SYSMOUNT_H */
