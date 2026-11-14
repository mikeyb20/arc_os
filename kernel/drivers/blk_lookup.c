/* arc_os — Block device lookups (M1-10, M1-13): the registry's published
 * entries read through blk_get, no lock (blkdev.c). Mount time only:
 * O(devices) each. Its own file so host suites can link it without the rest
 * of the block layer. */

#include "drivers/blkdev.h"
#include "lib/kprintf.h"
#include "lib/string.h"

BlockDevice *blk_find(const char *name) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

BlockDevice *blk_find_disk(const Guid *disk_guid) {
    BlockDevice *d, *found = NULL;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno != 0 || !d->has_gpt || !gpt_guid_equal(&d->disk_guid, disk_guid)) continue;
        if (found == NULL) {
            found = d;
        } else {   /* a cloned disk (dd): the GUID can't tell them apart */
            char text[GUID_STR_LEN];
            gpt_guid_format(disk_guid, text);
            kprintf("[BLK] WARNING: %s and %s share GPT GUID %s; using %s\n", found->name, d->name, text, found->name);
        }
    }
    return found;
}

BlockDevice *blk_find_part(const BlockDevice *disk, const Guid *type) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno != 0 && d->disk == disk && gpt_guid_equal(&d->type_guid, type)) return d;
    }
    return NULL;
}

BlockDevice *blk_find_label(const char *label, const Guid *type) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno == 0 || strcmp(d->label, label) != 0) continue;
        if (type == NULL || gpt_guid_equal(&d->type_guid, type)) return d;
    }
    return NULL;
}

BlockDevice *blk_find_partuuid(const Guid *part_guid) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno != 0 && gpt_guid_equal(&d->part_guid, part_guid)) return d;
    }
    return NULL;
}
