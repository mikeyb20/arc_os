# ADR-0019: Efficiency rules, budgets and kernel allocators

- **Status:** Accepted (Michael, 2026-11-08)
- **Date:** 2026-11-08
- **Refines:** ADR-0016 (descriptor size is now fixed, not a target) and ADR-0004 (large file pages stay later, but
  the structures are ready for them). Neither is superseded.

## Context
A review of master (a06ab25) and the M1 specs before any M1 code was written found:
- The kernel heap had no lock, yet an interrupt handler allocates from it (virtio-net rx → TCP input → `kmalloc`).
  Fixed in the same PR as this ADR.
- The plan had no object allocator. `kmalloc` stays a first-fit list that walks every heap block, while M1 adds
  thousands of small objects (VMAs, inodes, dentries, block requests, page-cache tree nodes).
- The M1-02 spec allowed a 64-byte `struct page`; ADR-0016 targets 32 bytes. At 4 GB that is 64 MiB vs 32 MiB.
- The page cache was specified for 4 KiB pages only. Adding large file pages to a 4 KiB-only page cache later is a
  deep rewrite (Linux's folio conversion took years).
- Nothing checked efficiency before M2-06. Cloud QEMU runs in TCG (10–50× slower, noisy), so CI cannot time code.

## Decision
1. **Cost lines.** Every deep (D) block spec has a **Cost.** section: the hot paths it adds, their target
   complexity, and bytes per object. Review checks the code against it. Planning blocks M2-00 … M5-00 write them.
2. **Efficiency rules** in `docs/plans/work-blocks.md` §7 bind every kernel block: no scan of a global list or array
   in allocate, free, fault, schedule, lookup or interrupt paths; locks taken from interrupts are IRQ-safe; no
   allocation in hard-IRQ context once M1-09 lands; hot structures carry `_Static_assert` on their size; hot paths
   log at debug level only.
3. **Budgets in CI.** A checked-in budgets file holds limits for counts and sizes only (never time). The QEMU harness
   reads `ARC_TEST budget` lines from the kernel and fails when a value exceeds its limit by more than 5%. Raising a
   limit is a reviewed change with a reason. Block M0-08 builds it.
4. **Host tests count steps, not time,** for core structures (allocators, VMA tree, page-cache tree, run queues):
   they assert worst-case steps per operation.
5. **Real timing only on hardware,** at the checkpoints the plan already has (M1-04, M1-22, M4-14, M5-06): an
   `arc-bench` program (context switch, page fault, 4 KiB and 2 MiB allocation, sequential read, mmap fault
   throughput), results committed under `docs/perf/`. M2-06 adds llama.cpp tokens/s.
6. **Slab allocator** (block M1-02b, before M1-05): per-size and named object caches on top of the buddy allocator;
   `kmalloc` becomes size classes over it, with no per-object header. Per-CPU object caches are later and additive.
7. **`struct page` is exactly 32 bytes** and reserves compound-page fields (order, head/tail). The buddy allocator's
   only interface is `alloc_pages(order, flags)` / `free_pages(page, order)`, so per-CPU page lists can be added
   later without changing callers.
8. **The page-cache interface never assumes order-0 pages:** lookups return a page plus an offset inside it. M1-12
   builds order 0 only. M2-06 measures llama.cpp with `mmap` (4 KiB file pages) against loading into 2 MiB
   anonymous pages and records which the `arc-llm` service uses.

## Consequences
- Adds blocks M0-08 (S) and M1-02b (M): +1.5 session units, about +2 sessions with the allowance.
- Rules out: O(n) scans in hot paths, per-page data larger than 32 bytes, timing-based CI gates, a page-cache API
  that bakes in 4 KiB.

## Verification
- `tests/test_kmalloc.c` checks the heap lock; M1-02 `_Static_assert(sizeof(struct page) == 32)`.
- M0-08: CI fails on a deliberately exceeded budget.
- M1-02b host test: 100 000 mixed allocations with bounded steps per operation.
- `docs/perf/` has a result file from each hardware checkpoint.
