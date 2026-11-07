# ADR-0008: GPT disk layout and ext2

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
FAT32 caps files at 4 GB and is read-only in arc_os. Models and packs are larger. We need a writable filesystem that
Linux host tools can build and check.

## Decision
- Disks are GPT. Partitions: **ESP** (FAT32, Limine + kernel), **system** (ext2), **data** (ext2). Sizes are fixed in M5.
- ext2 revision 1, 4 KiB blocks, `large_file` and `sparse_super`, read/write. No journal yet; ext3/ext4 journaling is a later ADR.
- Durable update rule: write to a temp file in the same directory, verify, `fsync`, `rename` over the target.
- FAT32 stays for the ESP only.

## Consequences
- Images are built and checked on Linux with standard tools (`mkfs.ext2`, `e2fsck`, `debugfs`).
- If the PC dies, the data disk is readable on any Linux machine.
- Rules out: a custom on-disk filesystem for now; storing packs on FAT32.

## Verification
- `e2fsck -fn` passes on images written by arc_os; QEMU test writes, renames and re-reads large files.
