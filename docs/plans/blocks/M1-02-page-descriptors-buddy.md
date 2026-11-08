# M1-02 — Page descriptors and buddy allocator

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M0-02 | M1-02b, M1-04, M1-07 |

**Goal.** Every physical page has a descriptor allocated from the PMM at boot, and physical memory comes from a
buddy allocator that can hand out 2 MiB blocks (ADR-0016, ADR-0004).

**Read first.**
- `docs/decisions/0016-kernel-memory-sizing.md`, `0004-page-cache-and-mmap.md`
- `kernel/mm/pmm.c`, `pmm.h`, `vmm.h`, `kmalloc.c`, `kmalloc.h`, `kernel/boot/bootinfo.h`, `tests/test_pmm.c`

**Build.**
1. `struct page`, exactly 32 bytes (`_Static_assert`, ADR-0019): u32 flags (with the order and head/tail bits for
   compound pages), u32 refcount, u32 mapcount, u32 index, an 8-byte mapping pointer (union: slab cache, M1-02b),
   and list links as two u32 PFNs (union: slab free list). 32-bit PFNs cover 16 TiB of RAM. `pfn_to_page`, `page_to_pfn`, `page_to_virt` (HHDM).
2. The descriptor array covers PFN 0 to the highest usable PFN. It is carved from the memory map before any other
   allocation; holes and reserved ranges are marked reserved.
3. Buddy allocator, orders 0–10 (4 KiB–4 MiB), per-order free lists, split and coalesce:
   `alloc_pages(order, flags)`, `free_pages(page, order)`. These are the only interface, so per-CPU page lists can be
   added later without changing callers (ADR-0019). Keep `pmm_alloc_page()` and friends as thin wrappers
   so callers don't change in this block. The bitmap goes away.
4. A `below 4 GiB` allocation flag for 32-bit DMA devices, so drivers never need a rework.
5. kmalloc: cap at 512 MB (ADR-0016); large allocations go straight to `alloc_pages`.
6. Kernel command line `arc.mem=<size>` ignores RAM above the cap, so a bigger machine can be tested at the 4 GB
   floor (the low-tier test machine may have more than 4 GB).
7. `/proc/meminfo`: MemTotal, MemFree, descriptor overhead.
8. Host tests: split/coalesce, order-9 still available after fragmentation, holes, exhaustion. QEMU: all suites
   with `-m 4G` (memory hole below 4 GiB, RAM above it).

**Done when.**
- [x] Host and QEMU suites pass.
- [x] The boot log prints the descriptor array size and free blocks per order; a 2 MiB allocation succeeds after
      boot with 4 GB of RAM.

**Cost.** `alloc_pages`/`free_pages` at most 11 steps (one per order); `pfn_to_page` O(1); descriptors 32 B per page (32 MiB at 4 GB, 256 MiB at 32 GB).

**Watch out.**
- 32 bytes is a hard limit; if a field doesn't fit, it goes in a union or a side table, never a bigger struct.
- Bootstrap order: nothing may allocate before the array exists, so the array is placed straight from the
  memory map.
- Keep PFN 0 and memory below 1 MiB reserved.
- On real machines the highest PFN can sit well above the RAM size (holes). Log the overhead for usable pages.

**Notes.**
- 2026-11-08, done (WIP parked after PR #5, rebased onto master after M1-01). Files: `mm/page.h` (`struct page`,
  32 B, `_Static_assert`; PFN/phys/HHDM inlines), `mm/gfp.h` (`GFP_KERNEL`, `GFP_ZERO`, `GFP_DMA32`), `mm/pmm.c`
  rewritten (bitmap gone), `kmalloc.c` large path, `/proc/meminfo` `PageDescriptors` + `HeapLarge`.
- Buddy: orders 0–10, two zones (DMA32 below 4 GiB, NORMAL above; a 4 MiB block never crosses 4 GiB). Each zone has
  a `nonempty` bitmask, so finding the smallest fitting order is one `ctz`. Free lists are doubly linked through the
  descriptors' u32 PFN links: free pages are never written, so the allocator works whether or not a page is mapped.
  Without `GFP_DMA32`, memory above 4 GiB goes first. One IRQ-safe leaf lock (§7 level 10).
- Descriptor array: placed top-down in the highest usable range that holds it. On q35 `-m 4G` the array covers
  6 GiB of PFNs (2 GiB sits above 4 GiB): 48 MiB, 1.17 % (the spec's 32 MiB assumes no hole). A sparse array later
  would change only `pfn_to_page` internals.
- `arc.mem=<size>` is an amount of RAM (counted from 1 MiB up, descriptor array included), not an address: `arc.mem=4G`
  on a 6 GiB guest uses exactly 4096 MiB (`qemu.mem.cap`). Below 16M it is ignored with a warning.
- kmalloc: 8 KiB–4 MiB go to `alloc_pages` rounded up to a power of two, with `PG_KMALLOC` on the head; `kfree`
  panics on a page-allocator block that lacks it. One 512 MB cap covers both paths. Over 4 MiB still comes from the
  first-fit heap (no caller does that today; M1-02b must keep it failing cleanly). `GFP_DMA32` is ignored by kmalloc.
- Budgets: `kernel_heap_bytes` and the init leak check now count large blocks (`HeapLarge`). Rounding costs 467 KB at
  boot, 432 KB of it on 7 ramfs nodes of 33 792 B (→ 64 KiB) and 6 VT buffers of 96 000 B (→ 128 KiB); limit 2.8 MB. Worth
  sizing those buffers when their owners are next edited. New `page_descriptor_bytes` budget; `kmalloc_walk_max`
  fell to 84 (limit 92).
- ADR-0016's `-m 32G` boot can't run on 16 GB CI runners (QEMU can't reserve the RAM); host test `sizing_32g` checks
  the array math for the HIGH machine's layout instead.
- For M1-02b: `struct page` unions `mapping`/`slab_cache` and `lru`/`slab_free` are ready; flag bits 9–31 are free
  (`free_pages` clears all flags). `in_heap()` in kmalloc.c is the heap/large switch the slab replaces.
- Seen, not this block: the VMM maps the HHDM up to the highest memory-map address, which on q35 is 1 TiB (a reserved
  entry near 1 TiB), costing 4 MiB of page tables (1024 page directories); M1-07 or M1-08 should map only RAM.
