#!/bin/bash
# Create a small FAT32 disk image for host-side tests using mtools (no root required).
set -e

IMG="${1:-tests/fat32_test.img}"

dd if=/dev/zero of="$IMG" bs=1M count=34 2>/dev/null
mkfs.fat -F 32 -n "TESTDISK" "$IMG" >/dev/null
mmd -i "$IMG" ::subdir
echo -n "Hello, FAT32!" | mcopy -i "$IMG" - ::hello.txt
echo -n "1234567890" | mcopy -i "$IMG" - ::subdir/nested.txt
# Several clusters (the read path walks the chain) and a read-only entry (M1-11)
seq 1 3000 | mcopy -i "$IMG" - ::big.txt
echo -n "ro" | mcopy -i "$IMG" - ::ro.txt
mattrib -i "$IMG" +r ::ro.txt
echo "Created FAT32 test image: $IMG"
