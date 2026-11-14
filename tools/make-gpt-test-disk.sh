#!/bin/bash
# GPT test disks for the block layer (M1-10), no root needed.
#
#   make-gpt-test-disk.sh host DIR      host-test images in DIR:
#       gpt512.img            sgdisk: arc-system, arc-data and a UTF-8 label
#       gpt512-badhdr.img     the same with the primary header's CRC broken
#       gpt512-badents.img    the same with a primary entry changed (entries CRC)
#       gpt4k.img             4096-byte blocks (make-gpt-image.py), same layout
#   make-gpt-test-disk.sh devices IMG   QEMU suite "devices": partition 1 = ESP
#       holding the FAT32 test volume, partition 2 = "blktest", where every
#       64-bit word of sector s (from the partition start) is s
#   make-gpt-test-disk.sh writeback IMG   QEMU suites "writeback" (M1-14):
#       partition 1 = "wbtest", 1 MiB of zeros the kernel's write-back test
#       writes in its first boot and reads back from the disk in the second
#
# Type GUIDs and labels come from kernel/include/arc_disk.h (one home).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HDR="$ROOT/kernel/include/arc_disk.h"
def() { sed -n "s/^#define $1 *\"\(.*\)\"/\1/p" "$HDR"; }
SYSTEM=$(def ARC_PART_TYPE_SYSTEM)
DATA=$(def ARC_PART_TYPE_DATA)
ESP=$(def ARC_PART_TYPE_ESP)
LSYS=$(def ARC_PART_LABEL_SYSTEM)
LDATA=$(def ARC_PART_LABEL_DATA)
LINUX=0FC63DAF-8483-4772-8E79-3D69D8477DE4
DISK=4A7D3F10-1B2C-4D5E-8F90-A1B2C3D4E5F6
P1=11111111-2222-4333-8444-555555555551
P2=11111111-2222-4333-8444-555555555552
P3=11111111-2222-4333-8444-555555555553
UTF8_LABEL="données-✓"

host() {
    local dir="$1"
    mkdir -p "$dir"
    local img="$dir/gpt512.img"
    rm -f "$img"
    truncate -s 8M "$img"
    sgdisk -o -U "$DISK" \
        -n 1:2048:4095 -t 1:"$SYSTEM" -u 1:"$P1" -c 1:"$LSYS" \
        -n 2:4096:8191 -t 2:"$DATA" -u 2:"$P2" -c 2:"$LDATA" \
        -n 3:8192:10239 -t 3:"$LINUX" -u 3:"$P3" -c 3:"$UTF8_LABEL" "$img" >/dev/null
    # Primary header CRC (LBA 1, offset 16) and first entry's first LBA (LBA 2 + 32)
    cp "$img" "$dir/gpt512-badhdr.img"
    printf '\xde\xad' | dd of="$dir/gpt512-badhdr.img" bs=1 seek=$((512 + 16)) conv=notrunc 2>/dev/null
    cp "$img" "$dir/gpt512-badents.img"
    printf '\x07' | dd of="$dir/gpt512-badents.img" bs=1 seek=$((1024 + 32)) conv=notrunc 2>/dev/null
    # 4Kn: the same partitions in 4 KiB blocks (2048 blocks = 8 MiB)
    local parts=(--part "256:511:$SYSTEM:$P1:$LSYS" --part "512:1023:$DATA:$P2:$LDATA"
                 --part "1024:1279:$LINUX:$P3:$UTF8_LABEL")
    python3 "$ROOT/tools/make-gpt-image.py" "$dir/gpt4k.img" --block 4096 --blocks 2048 --disk-guid "$DISK" "${parts[@]}"
    # The writer's 512-byte output must satisfy sgdisk too
    python3 "$ROOT/tools/make-gpt-image.py" "$dir/check512.img" --block 512 --blocks 16384 --disk-guid "$DISK" \
        --part "2048:4095:$SYSTEM:$P1:$LSYS"
    sgdisk -v "$dir/check512.img" | grep -q "No problems found" || { echo "make-gpt-image.py output fails sgdisk -v"; exit 1; }
    rm -f "$dir/check512.img"
    echo "Created GPT test images in $dir"
}

devices() {
    local img="$1" tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    bash "$ROOT/tools/make-fat32-test-disk.sh" "$tmp/fat.img" >/dev/null
    rm -f "$img"
    truncate -s 40M "$img"
    sgdisk -o -U "$DISK" -n 1:2048:+34M -t 1:"$ESP" -c 1:"EFI system" \
        -n 2:0:+4M -t 2:"$LINUX" -c 2:blktest "$img" >/dev/null
    dd if="$tmp/fat.img" of="$img" bs=512 seek=2048 conv=notrunc 2>/dev/null
    local first
    first=$(sgdisk -i 2 "$img" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    python3 - "$img" "$first" <<'EOF'
import struct, sys
path, first = sys.argv[1], int(sys.argv[2])
with open(path, "r+b") as f:
    f.seek(first * 512)
    f.write(b"".join(struct.pack("<Q", s) * 64 for s in range(8192)))
EOF
    echo "Created GPT devices test disk: $img"
}

writeback() {
    local img="$1"
    rm -f "$img"
    truncate -s 4M "$img"
    sgdisk -o -U "$DISK" -n 1:2048:+1M -t 1:"$LINUX" -c 1:wbtest "$img" >/dev/null
    echo "Created GPT write-back test disk: $img"
}

case "$1" in
host) host "$2" ;;
devices) devices "$2" ;;
writeback) writeback "$2" ;;
*) echo "usage: $0 host DIR | devices IMG | writeback IMG" >&2; exit 2 ;;
esac
