# ADR-0016: Per-page metadata from the PMM at boot

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
The PMM is a bitmap with no per-page data. The page cache, COW and mmap need a refcount and flags per physical page.
At 16 bytes per 4 KiB page, 64 GB of RAM needs 256 MB of metadata, while the kernel heap is capped at 512 MB.

## Decision
- At boot, after reading the memory map, the PMM allocates one contiguous array of page descriptors covering all usable
  RAM directly from physical memory, reached through the HHDM. It is never allocated from kmalloc.
- Each descriptor is a fixed small struct (target ≤ 32 bytes): refcount, flags, owner (page cache mapping + index, or
  anonymous), LRU links.
- `pfn_to_page()` / `page_to_pfn()` are O(1).
- The bitmap allocator may stay as the free-page index or be replaced by a buddy allocator; either way the descriptor
  array is the source of truth for page state.

## Consequences
- Memory metadata scales with RAM without touching the heap. Rules out: per-page data in kmalloc'd structures.

## Verification
- Host tests for descriptor sizing and pfn math; boot log prints metadata size; QEMU boots with `-m 4G` and `-m 32G`.
