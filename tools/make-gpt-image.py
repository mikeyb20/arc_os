#!/usr/bin/env python3
"""Write a GPT disk image with any logical block size (M1-10 host tests).

sgdisk can't set the sector size of an image file (it assumes 512), so the
4Kn test image comes from here. Its 512-byte output is checked with
`sgdisk -v` by tools/make-gpt-test-disk.sh, so the two writers agree.

usage: make-gpt-image.py OUT --block 4096 --blocks N --disk-guid G
                         --part FIRST:LAST:TYPE:GUID:LABEL ...
"""
import argparse
import struct
import uuid
import zlib


def guid_bytes(text):
    return uuid.UUID(text).bytes_le


def build(block, blocks, disk_guid, parts):
    n_entries, entry_size = 128, 128
    entry_blocks = n_entries * entry_size // block
    entries = bytearray(n_entries * entry_size)
    for i, (first, last, ptype, pguid, label) in enumerate(parts):
        name = label.encode("utf-16-le")[:72].ljust(72, b"\0")
        entries[i * 128:(i + 1) * 128] = (guid_bytes(ptype) + guid_bytes(pguid) +
                                          struct.pack("<QQQ", first, last, 0) + name)
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF
    first_usable = 2 + entry_blocks
    last_usable = blocks - 2 - entry_blocks

    def header(my_lba, alt_lba, entries_lba):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, my_lba, alt_lba,
                        first_usable, last_usable, guid_bytes(disk_guid), entries_lba, n_entries,
                        entry_size, entries_crc)
        crc = zlib.crc32(h) & 0xFFFFFFFF
        h = h[:16] + struct.pack("<I", crc) + h[20:]
        return h.ljust(block, b"\0")

    img = bytearray(block * blocks)
    mbr = bytearray(512)
    size = min(blocks - 1, 0xFFFFFFFF)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, size)
    mbr[510:512] = b"\x55\xaa"
    img[0:512] = mbr
    img[block:2 * block] = header(1, blocks - 1, 2)
    img[2 * block:2 * block + len(entries)] = entries
    backup_entries = blocks - 1 - entry_blocks
    img[backup_entries * block:backup_entries * block + len(entries)] = entries
    img[(blocks - 1) * block:] = header(blocks - 1, 1, backup_entries)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--block", type=int, default=512)
    ap.add_argument("--blocks", type=int, required=True)
    ap.add_argument("--disk-guid", required=True)
    ap.add_argument("--part", action="append", default=[])
    a = ap.parse_args()
    parts = []
    for p in a.part:
        first, last, ptype, pguid, label = p.split(":", 4)
        parts.append((int(first), int(last), ptype, pguid, label))
    with open(a.out, "wb") as f:
        f.write(build(a.block, a.blocks, a.disk_guid, parts))


if __name__ == "__main__":
    main()
