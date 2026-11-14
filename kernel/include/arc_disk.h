#ifndef ARCHOS_INCLUDE_ARC_DISK_H
#define ARCHOS_INCLUDE_ARC_DISK_H

/* arc_os disk layout constants (ADR-0008, ADR-0018): GPT partition type GUIDs
 * and labels. The one home for them: the kernel parses these strings
 * (gpt_guid_parse) and the host image tools read this file too
 * (tools/make-gpt-test-disk.sh pulls the values out with sed), so keep each
 * on one line as `#define NAME "value"`. GUIDs are in the usual text form;
 * GPT stores the first three fields little-endian. */

/* arc_os partitions (ADR-0018) */
#define ARC_PART_TYPE_SYSTEM  "0BD6D742-55DB-4D77-8646-D17A35F4BE85"
#define ARC_PART_TYPE_DATA    "AFFB8E56-A8BE-4817-9A15-F945F7238591"
#define ARC_PART_LABEL_SYSTEM "arc-system"
#define ARC_PART_LABEL_DATA   "arc-data"

/* EFI system partition (FAT32: Limine and the kernel, ADR-0008) */
#define ARC_PART_TYPE_ESP "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

#endif /* ARCHOS_INCLUDE_ARC_DISK_H */
