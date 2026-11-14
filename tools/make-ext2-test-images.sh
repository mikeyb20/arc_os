#!/bin/bash
# Build the ext2 images the host ext2 suite reads (M1-13), no root needed:
#   tests/ext2_4k.img  4 KiB blocks, 256-byte inodes (the arc_os layout)
#   tests/ext2_2k.img  2 KiB blocks, 256-byte inodes
#   tests/ext2_1k.img  1 KiB blocks, 128-byte inodes
# All hold the same tree; tests/test_ext2.c knows its contents. Needs
# e2fsprogs (mkfs.ext2, e2fsck, debugfs) and python3.
set -e

OUT="${1:-tests}"
SRC="$(mktemp -d)"
trap 'rm -rf "$SRC"' EXIT

python3 - "$SRC" <<'PY'
import os, sys
root = sys.argv[1]
def pat(off, n):
    # The byte at file offset i: test_ext2.c computes the same
    return bytes(((i * 7) ^ (i >> 12) ^ (i >> 20) ^ (i >> 28)) & 0xFF for i in range(off, off + n))
def w(path, data=b""):
    p = os.path.join(root, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)
w("hello.txt", b"Hello, ext2!\n")
w("empty")
w("dir/a/b/c/deep.txt", b"deep\n")
w("medium.bin", pat(0, 300000))          # direct, single and (1 KiB blocks) double indirect
# Sparse: 4 KiB of pattern at offsets in each block-map level, holes between,
# the last bytes just under 5 GiB (triple indirect at every block size)
offs = [0, 49152, 4243456, 1 << 30, (5 << 30) - 4096]
with open(os.path.join(root, "sparse.bin"), "wb") as f:
    for o in offs:
        f.seek(o)
        f.write(pat(o, 4096))
long_dir = "d" * 40 + "/" + "e" * 40
w(long_dir + "/target.txt", b"slow target\n")
os.symlink("hello.txt", os.path.join(root, "link_fast"))
os.symlink(long_dir + "/target.txt", os.path.join(root, "link_slow"))
os.symlink("../../hello.txt", os.path.join(root, "dir/a/up"))
os.symlink("nowhere", os.path.join(root, "link_dangling"))
os.symlink("loop_b", os.path.join(root, "loop_a"))
os.symlink("loop_a", os.path.join(root, "loop_b"))
os.symlink("dir/a", os.path.join(root, "link_dir"))
os.makedirs(os.path.join(root, "many"))
for i in range(400):
    w("many/file_%03d_with_a_longish_name" % i, b"%d\n" % i)
os.mkfifo(os.path.join(root, "fifo"))
os.chmod(os.path.join(root, "hello.txt"), 0o640)
PY

make_img() {
    local img="$1" bs="$2" isz="$3"
    rm -f "$img"
    mkfs.ext2 -q -F -t ext2 -b "$bs" -I "$isz" -O large_file -L arc-test -d "$SRC" "$img" 16M
    # Index the directories (dir_index htree), as a Linux-written volume has
    e2fsck -fyD "$img" >/dev/null 2>&1 || [ $? -le 1 ]
    debugfs -R "stat many" "$img" 2>/dev/null | grep -q "Flags: 0x1000" ||
        { echo "$img: 'many' has no htree index" >&2; exit 1; }
}
make_img "$OUT/ext2_4k.img" 4096 256
make_img "$OUT/ext2_2k.img" 2048 256
make_img "$OUT/ext2_1k.img" 1024 128
echo "Created ext2 test images in $OUT"
