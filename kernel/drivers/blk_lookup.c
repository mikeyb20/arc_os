/* arc_os — Block device lookups (M1-10, M1-13): the registry's published
 * entries read through blk_get, no lock (blkdev.c). Mount time only:
 * O(devices) each. Its own file so host suites can link it without the rest
 * of the block layer. */

#include "drivers/blkdev.h"
#include "lib/string.h"

BlockDevice *blk_find(const char *name) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

BlockDevice *blk_find_disk(const Guid *disk_guid) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno == 0 && d->has_gpt && gpt_guid_equal(&d->disk_guid, disk_guid)) return d;
    }
    return NULL;
}

BlockDevice *blk_find_part(const Guid *disk_guid, const Guid *type) {
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->partno != 0 && gpt_guid_equal(&d->disk_guid, disk_guid) && gpt_guid_equal(&d->type_guid, type)) return d;
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
