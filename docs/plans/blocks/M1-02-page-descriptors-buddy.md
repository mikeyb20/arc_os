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
- [ ] Host and QEMU suites pass.
- [ ] The boot log prints the descriptor array size and free blocks per order; a 2 MiB allocation succeeds after
      boot with 4 GB of RAM.

**Cost.** `alloc_pages`/`free_pages` at most 11 steps (one per order); `pfn_to_page` O(1); descriptors 32 B per page (32 MiB at 4 GB, 256 MiB at 32 GB).

**Watch out.**
- 32 bytes is a hard limit; if a field doesn't fit, it goes in a union or a side table, never a bigger struct.
- Bootstrap order: nothing may allocate before the array exists, so the array is placed straight from the
  memory map.
- Keep PFN 0 and memory below 1 MiB reserved.
- On real machines the highest PFN can sit well above the RAM size (holes). Log the overhead for usable pages.

**Notes.**
-
