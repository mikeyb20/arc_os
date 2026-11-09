# M1-02b — Slab allocator

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-02 | M1-05 |

**Goal.** Small kernel objects come from slab caches on top of the buddy allocator, so `kmalloc` and `kfree` take
constant time and carry no per-object header (ADR-0019).

**Read first.**
- `docs/decisions/0019-efficiency-budgets-and-allocators.md`, `0016-kernel-memory-sizing.md`
- `kernel/mm/kmalloc.c`, `kmalloc.h`, the page-descriptor header from M1-02, `tests/test_kmalloc.c`
- Notes of M1-02

**Build.**
1. Object caches: `kmem_cache_create(name, size, align)`, `kmem_cache_alloc(cache, flags)`,
   `kmem_cache_free(cache, obj)`, `kmem_cache_shrink(cache)`. A slab is one order-0 block (order 1–3 for objects over
   512 bytes) from `alloc_pages`, reached through the HHDM. Its `struct page` holds the cache pointer, the free-object
   list and the in-use count. (As built: the cache pointer is the `mapping` union; in-use count, never-used cursor and
   free-list head overlay `mapcount`/`index`, because the slab lists need the `lru` links that `slab_free` shared.) Each cache keeps partial, full and empty slab lists.
2. `kmalloc` on top: size classes 16, 32, 64, 128, 256, 512, 1024, 2048 bytes; larger requests go straight to
   `alloc_pages` at the smallest order that fits. `kfree` finds the owner through `virt → page → cache` in O(1).
   `krealloc` keeps its semantics. `GFP_ZERO` still works. The 512 MB cap of ADR-0016 becomes an accounting limit.
3. The first-fit heap, its 0xFFFFFFFFC0000000 window and `heap_grow` go away. Thread kernel stacks become order-2
   blocks (no guard page until M1-16).
4. One IRQ-safe lock per cache replaces the single heap lock. Per-CPU object caches are later and additive.
5. Empty slabs above a reserve of one per cache go back to the buddy allocator; `kmem_cache_shrink` is the hook
   reclaim (M1-14) calls.
6. Debug builds: red zones, poison on free, and a double-free check. Release builds: none of these.
7. `/proc/slabinfo`: name, object size, active objects, total objects, slabs.
8. Later blocks create named caches for their hot objects (VMA, inode, dentry, file, block request, page-cache
   tree node, futex waiter); their specs already say "from its slab cache".
9. Tests: host — every size class; 100 000 mixed alloc/free pairs with no overlap and a fixed bound on steps per
   operation (ADR-0019); empty slabs are returned; large allocations round-trip; `kfree(NULL)`. QEMU — all suites;
   free pages return to the boot baseline after them.

**Done when.**
- [x] Host and QEMU suites pass; the first-fit heap code is gone.
- [x] The boot log lists the caches; `/proc/slabinfo` reads.
- [x] The M0-08 budget for heap blocks walked per call (`kmalloc_walk_max`, `kmalloc_max_walk()`) is replaced by slab steps per call.

**Cost.** Allocate and free O(1); `kfree` owner lookup O(1) through `pfn_to_page`; no per-object header; idle
overhead at most one empty slab per cache.

**Watch out.**
- Bootstrap: the size-class caches are static, so creating them needs no allocation.
- Requests above order 10 (4 MiB) fail; grep for any caller that needs that much before deleting the heap.
- `kfree` of a pointer that no slab or page allocation owns panics in debug builds.

**Notes.**
- 2026-11-09, done. Files: `mm/slab.{c,h}` (new), `mm/kmalloc.{c,h}` rewritten (first-fit heap, its
  0xFFFFFFFFC0000000 window and `heap_grow` gone), `mm/page.h` (`PG_SLAB`, slab overlay on `mapcount`/`index`,
  `struct page_list` PFN-list helpers now shared with the buddy allocator), thread stacks = order-2 blocks.
- No loop on alloc/free: a new slab is not carved; `slab_fresh` hands out never-used objects in order, freed ones go
  on a u32 byte-offset list. Steps per call are 1-3 (budget `slab_steps_max` 3; the boot heap self-test now frees
  200 objects, so every suite hits the 3-step path and checks empty slabs go back to the buddy allocator).
- Multi-page slabs: every page carries `PG_SLAB` + cache pointer; the head is the PFN rounded down to the slab order.
  `slab_release` clears the tails first (buddy invariant: non-head pages of a free block have flags 0).
- Debug checks need `ARC_DEBUG` (new CMake option `ARC_KERNEL_DEBUG`, default ON; M1-05's lock checks should use the
  same switch). Red zone = 8 bytes after the object (state word + free link), so debug strides are bigger and some
  big classes get bigger slabs (kmalloc-1024: order 2, kmalloc-2048: order 3). `kfree` of an unowned pointer panics
  in every build (the owner check is the dispatch anyway).
- One 512 MB cap (ADR-0016) is now an atomic charge (`kmem_charge`) for slab pages + large blocks.
  `kernel_heap_bytes` 2 549 888 -> 2 551 808 at boot (large blocks still dominate, see M1-02 Notes).
- Size: ~800 code lines added (397 deleted) + ~575 test lines, past 1.5x M; not split since it was one piece.
- For later blocks: exec reads a whole ELF into one kmalloc buffer (`MAX_ELF_SIZE` 16 MB) and ramfs files grow by
  `krealloc` doubling, so both now stop at 4 MiB (no file that big today; static llama.cpp binaries in M2 will be).
  The FAT32 FAT cache is one kmalloc too (fine for an ESP). M1-16's stack guard page needs stacks outside the HHDM:
  only `thread_create`/`thread_destroy` change.
