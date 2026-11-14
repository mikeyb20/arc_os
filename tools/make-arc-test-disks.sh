#!/bin/bash
# Disks for the QEMU ext2 suites (M1-13), no root needed:
#
#   make-arc-test-disks.sh make DIR [HELLO]
#       DIR/arc-disk.img      an arc_os disk: GPT, partition 1 = system (ext2,
#                             type and label from kernel/include/arc_disk.h),
#                             partition 2 = data (apps, models, packs, home, var),
#                             partition 3 = an EFI system partition (FAT32)
#       DIR/foreign-disk.img  someone else's disk: a Linux partition labelled
#                             "arc-system" and a Windows one labelled "arc-data",
#                             both holding ext2 volumes, and its own EFI system
#                             partition, none of which arc_os may mount
#       DIR/arc-sums.txt      CRC-32 of every file ext2test reads ("crc PATH HEX")
#       DIR/foreign.sha256    the foreign disk's checksum before the run
#       HELLO, if given, is copied to /system/bin/hello (exec from ext2).
#   make-arc-test-disks.sh make-big DIR
#       DIR/big-disk.img      an arc_os disk for qemu.reclaim (M1-14): only a
#                             system partition, holding big.bin, a 2 GiB
#                             sparse file: in each 8 MiB stretch b, the first
#                             64 KiB are data (64-bit word j is b << 32 | j),
#                             the rest a hole (reclaimtest checks both)
#   make-arc-test-disks.sh make-write DIR
#       DIR/write-disk.img    an arc_os disk for qemu.ext2w (M1-15): a small
#                             system partition and a 1300 MiB data partition
#                             (PARTUUID $WRITE_DATA, named with arc.data=) with
#                             empty apps, models, packs, home/user and var
#   make-arc-test-disks.sh check-write DIR first|second SERIAL_LOG
#       After a qemu.ext2w boot: the data partition is e2fsck-clean and marked
#       clean; after the first, /var/big.bin is 1 GiB; after the second, it is
#       gone and every block but the 103 the remaining files hold is free.
#   make-arc-test-disks.sh check DIR SERIAL_LOG
#       After the run: ext2test's crc lines match arc-sums.txt, and the foreign
#       disk is byte for byte what it was (it is attached writable).
#   make-arc-test-disks.sh check-foreign DIR SERIAL_LOG
#       Only the foreign disk.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HDR="$ROOT/kernel/include/arc_disk.h"
def() { sed -n "s/^#define $1 *\"\(.*\)\"/\1/p" "$HDR"; }
LINUX=0FC63DAF-8483-4772-8E79-3D69D8477DE4
WINDOWS=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
ARC_DISK=5A6B7C8D-1E2F-4A3B-8C4D-5E6F7A8B9C0D
FOREIGN_DISK=6B7C8D9E-2F3A-4B4C-9D5E-6F7A8B9C0D1E
# The foreign Windows partition: qemu.ext2.named names it with arc.data=PARTUUID=
FOREIGN_P2=22222222-3333-4444-8555-666666666662
# The write disk's data partition: qemu.ext2w names it with arc.data=PARTUUID=
WRITE_DATA=33333333-4444-4555-8666-777777777772

make_disks() {
    local dir="$1" hello="$2" tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$dir"
    python3 - "$tmp" "$dir/arc-sums.txt" "$hello" <<'PY'
import os, shutil, sys, zlib
tmp, sums, hello = sys.argv[1], sys.argv[2], sys.argv[3]
def pat(off, n):
    # The byte at file offset i (tests/test_ext2.c uses the same pattern)
    return bytes(((i * 7) ^ (i >> 12) ^ (i >> 20) ^ (i >> 28)) & 0xFF for i in range(off, off + n))
lines = []
def w(root, rel, data, sumpath=None):
    p = os.path.join(tmp, root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)
    if sumpath:
        lines.append("crc %s %08x" % (sumpath, zlib.crc32(data)))
# System: small, nested, a 6 MiB file (direct to double indirect at 4 KiB
# blocks) and a 5 GiB sparse file (triple indirect at its end)
w("system", "hello.txt", b"Hello from the arc_os system partition\n", "/system/hello.txt")
w("system", "dir/sub/deep.txt", b"deep\n", "/system/dir/sub/deep.txt")
w("system", "big.bin", pat(0, 6 << 20), "/system/big.bin")
offs = [0, 49152, 4243456, 1 << 30, (5 << 30) - 4096]
with open(os.path.join(tmp, "system", "sparse.bin"), "wb") as f:
    for o in offs:
        f.seek(o)
        f.write(pat(o, 4096))
        lines.append("crc /system/sparse.bin@%d %08x" % (o, zlib.crc32(pat(o, 4096))))
os.symlink("dir/sub/deep.txt", os.path.join(tmp, "system", "link"))
if hello:
    os.makedirs(os.path.join(tmp, "system", "bin"))
    shutil.copy(hello, os.path.join(tmp, "system", "bin", "hello"))
    os.chmod(os.path.join(tmp, "system", "bin", "hello"), 0o755)
# Data: one file per directory, a medium one in packs
w("data", "apps/app.txt", b"an app\n", "/apps/app.txt")
w("data", "models/model.txt", b"a model\n", "/models/model.txt")
w("data", "packs/pack.bin", pat(0, 300000), "/packs/pack.bin")
w("data", "home/user/notes.txt", b"notes\n", "/home/user/notes.txt")
w("data", "var/log.txt", b"log\n", "/var/log.txt")
w("data", "not-mounted.txt", b"the data root itself is not mounted\n")
# Foreign volumes: anything mounting them would show these
w("foreign1", "FOREIGN.txt", b"linux\n")
w("foreign2", "apps/FOREIGN.txt", b"windows\n")
with open(sums, "w") as f:
    f.write("\n".join(lines) + "\n")
PY
    for v in system data foreign1 foreign2; do
        mkfs.ext2 -q -F -t ext2 -b 4096 -I 256 -O large_file -d "$tmp/$v" "$tmp/$v.img" 24M
    done
    # EFI system partitions (FAT32 needs 34 MiB at 512-byte clusters), one
    # file each: /disk shows which was mounted
    for v in arc foreign; do
        truncate -s 34M "$tmp/esp-$v.img"
        mkfs.fat -F 32 -n "ESP" "$tmp/esp-$v.img" >/dev/null
        echo "$v esp" | mcopy -i "$tmp/esp-$v.img" - "::$(echo "$v" | tr a-z A-Z).TXT"
    done
    # arc_os disk
    local img="$dir/arc-disk.img"
    rm -f "$img"
    truncate -s 88M "$img"
    sgdisk -o -U "$ARC_DISK" \
        -n 1:2048:+24M -t 1:"$(def ARC_PART_TYPE_SYSTEM)" -c 1:"$(def ARC_PART_LABEL_SYSTEM)" \
        -n 2:0:+24M -t 2:"$(def ARC_PART_TYPE_DATA)" -c 2:"$(def ARC_PART_LABEL_DATA)" \
        -n 3:0:+34M -t 3:"$(def ARC_PART_TYPE_ESP)" "$img" >/dev/null
    place "$img" 1 "$tmp/system.img"
    place "$img" 2 "$tmp/data.img"
    place "$img" 3 "$tmp/esp-arc.img"
    # Foreign disk: the labels say arc_os, the types don't
    img="$dir/foreign-disk.img"
    rm -f "$img"
    truncate -s 88M "$img"
    sgdisk -o -U "$FOREIGN_DISK" \
        -n 1:2048:+24M -t 1:"$LINUX" -c 1:"$(def ARC_PART_LABEL_SYSTEM)" \
        -n 2:0:+24M -t 2:"$WINDOWS" -u 2:"$FOREIGN_P2" -c 2:"$(def ARC_PART_LABEL_DATA)" \
        -n 3:0:+34M -t 3:"$(def ARC_PART_TYPE_ESP)" "$img" >/dev/null
    place "$img" 1 "$tmp/foreign1.img"
    place "$img" 2 "$tmp/foreign2.img"
    place "$img" 3 "$tmp/esp-foreign.img"
    sha256sum "$img" | cut -d' ' -f1 >"$dir/foreign.sha256"
    echo "Created arc_os and foreign test disks in $dir"
}

make_big() {
    local dir="$1" tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$dir" "$tmp/system"
    python3 - "$tmp/system/big.bin" <<'PY'
import struct, sys
STRETCH, DATA, SIZE = 8 << 20, 64 << 10, 2 << 30
with open(sys.argv[1], "wb") as f:
    for b in range(SIZE // STRETCH):
        f.seek(b * STRETCH)
        f.write(struct.pack("<%dQ" % (DATA // 8), *((b << 32) | j for j in range(DATA // 8))))
    f.truncate(SIZE)
PY
    mkfs.ext2 -q -F -t ext2 -b 4096 -I 256 -O large_file -d "$tmp/system" "$tmp/system.img" 32M
    local img="$dir/big-disk.img"
    rm -f "$img"
    truncate -s 40M "$img"
    sgdisk -o -U "$ARC_DISK" \
        -n 1:2048:+32M -t 1:"$(def ARC_PART_TYPE_SYSTEM)" -c 1:"$(def ARC_PART_LABEL_SYSTEM)" "$img" >/dev/null
    place "$img" 1 "$tmp/system.img"
    echo "Created the reclaim test disk $img"
}

make_write() {
    local dir="$1" tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$dir" "$tmp/system" "$tmp/data"/{apps,models,packs,home/user,var}
    echo "system" >"$tmp/system/hello.txt"
    mkfs.ext2 -q -F -t ext2 -b 4096 -I 256 -O large_file -d "$tmp/system" "$tmp/system.img" 24M
    mkfs.ext2 -q -F -t ext2 -b 4096 -I 256 -O large_file -d "$tmp/data" "$tmp/data.img" 1300M
    local img="$dir/write-disk.img"
    rm -f "$img"
    truncate -s 1340M "$img"
    sgdisk -o -U "$ARC_DISK" \
        -n 1:2048:+24M -t 1:"$(def ARC_PART_TYPE_SYSTEM)" -c 1:"$(def ARC_PART_LABEL_SYSTEM)" \
        -n 2:0:+1300M -t 2:"$(def ARC_PART_TYPE_DATA)" -u 2:"$WRITE_DATA" -c 2:"$(def ARC_PART_LABEL_DATA)" \
        "$img" >/dev/null
    place "$img" 1 "$tmp/system.img"
    place "$img" 2 "$tmp/data.img"
    dumpe2fs -h "$tmp/data.img" 2>/dev/null | sed -n 's/^Free blocks: *//p' >"$dir/write-free.txt"
    echo "Created the ext2 write test disk $img"
}

check_write() {
    local dir="$1" which="$2" tmp first count bad=0 out
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    first=$(sgdisk -i 2 "$dir/write-disk.img" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    count=$(( 1300 << 20 ))
    dd if="$dir/write-disk.img" of="$tmp/data.img" bs=1M iflag=skip_bytes,count_bytes skip=$(( first * 512 )) \
        count="$count" conv=sparse 2>/dev/null
    if ! out=$(e2fsck -fn "$tmp/data.img" 2>&1); then
        echo "$out" | head -40
        echo "check: e2fsck found errors on the data partition"
        bad=1
    fi
    if ! dumpe2fs -h "$tmp/data.img" 2>/dev/null | grep -q "^Filesystem state: *clean$"; then
        echo "check: the data partition is not marked clean"
        bad=1
    fi
    local size
    size=$(debugfs -R "stat /var/big.bin" "$tmp/data.img" 2>/dev/null | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)
    if [ "$which" = first ] && [ "$size" != 1073741824 ]; then
        echo "check: /var/big.bin is '$size' bytes, not 1 GiB"
        bad=1
    fi
    if [ "$which" = second ]; then
        if [ -n "$size" ]; then
            echo "check: /var/big.bin still there"
            bad=1
        fi
        local free now
        free=$(cat "$dir/write-free.txt")
        now=$(dumpe2fs -h "$tmp/data.img" 2>/dev/null | sed -n 's/^Free blocks: *//p')
        # The gigabyte and its indirect blocks are back; what is left is the
        # 100 notes, the docs and sub directories and /var/state (a block each)
        if [ $(( free - now )) -ne 103 ]; then
            echo "check: free blocks $now, $free at the start: $(( free - now )) in use, not 103"
            bad=1
        fi
    fi
    [ $bad = 0 ] && echo "check: data partition e2fsck-clean and marked clean ($which boot)"
    return $bad
}

# Copy a volume image into partition n of a disk image
place() {
    local first
    first=$(sgdisk -i "$2" "$1" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    dd if="$3" of="$1" bs=512 seek="$first" conv=notrunc,sparse 2>/dev/null   # the disk is new: zeros stay holes
}

check() {
    local dir="$1" serial="$2" crcs="$3" bad=0
    while [ "$crcs" = 1 ] && read -r tag path sum; do
        [ "$tag" = crc ] || continue
        if ! grep -q "^ext2test: crc $path $sum\$" "$serial"; then
            echo "check: $path: expected crc $sum, got: $(grep "^ext2test: crc $path " "$serial" | head -1)"
            bad=1
        fi
    done <"$dir/arc-sums.txt"
    if [ "$(sha256sum "$dir/foreign-disk.img" | cut -d' ' -f1)" != "$(cat "$dir/foreign.sha256")" ]; then
        echo "check: the foreign disk changed"
        bad=1
    fi
    [ $bad = 0 ] && echo "check: foreign disk unchanged$([ "$crcs" = 1 ] && echo "; checksums match the host")"
    return $bad
}

case "$1" in
make) make_disks "$2" "$3" ;;
make-big) make_big "$2" ;;
make-write) make_write "$2" ;;
check-write) check_write "$2" "$3" ;;
check) check "$2" "$3" 1 ;;
check-foreign) check "$2" "$3" 0 ;;
*) echo "usage: $0 make DIR [HELLO] | make-big DIR | make-write DIR | check DIR SERIAL_LOG |" \
        "check-foreign DIR SERIAL_LOG | check-write DIR first|second SERIAL_LOG" >&2; exit 2 ;;
esac
