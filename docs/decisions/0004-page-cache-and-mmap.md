# ADR-0004: Unified page cache, demand paging, mmap, COW

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
LLM weights (GBs) and search indexes are memory-mapped and are larger than RAM on the low tier. Adding a page cache
after the VFS read path is built means rewriting it.

## Decision
- One page cache keyed by (inode, page index). `read`/`write` and file-backed `mmap` share the same physical pages.
- Demand paging: a VMA list per address space; page faults populate pages lazily (anonymous zero pages or file pages).
- `mmap`/`munmap`/`mprotect`/`madvise`/`msync` with Linux semantics; `MAP_PRIVATE` file mappings are copy-on-write.
- `fork` uses copy-on-write for all private memory.
- Reclaim: clean file pages are evictable (LRU-ish list); dirty pages are written back first. No swap.
- 4 KiB pages everywhere, 2 MiB pages for large aligned anonymous mappings and, later, page-cache-backed model files.
- Per-page metadata (refcount, flags, mapping) lives in the array defined by ADR-0016.

## Consequences
- A 10 GB file can be mapped and read on a 4 GB machine.
- The existing "copy everything" fork and direct VFS reads are replaced.
- Rules out: separate caches for read and mmap; eager loading of executables.

## Verification
- M1 done-test: memory-map a 10 GB ext2 file with 4 GB RAM and checksum it end to end.
- Host tests for VMA split/merge and COW refcounting.
