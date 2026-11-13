# M1-08 — Copy-on-write fork and 2 MiB pages

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-07 | M1-22 |

**Goal.** fork shares memory copy-on-write, and large anonymous areas use 2 MiB pages when possible (ADR-0004).

**Read first.**
- `docs/decisions/0004-page-cache-and-mmap.md`
- `kernel/mm/vmm.c` (fork path), the mm/VMA code and page-fault handler from M1-07, Notes of M1-07

**Build.**
1. fork: duplicate the VMAs; private writable pages become read-only in both processes with refcount + 1; a write
   fault copies the page, or just makes it writable again when the refcount is 1.
2. Page-table code handles 2 MiB leaf entries in walk, unmap, protect, fork and fault paths, and splits a 2 MiB
   mapping into 512 PTEs when part of it is unmapped or protected.
3. Anonymous huge pages: on the first touch of an aligned 2 MiB range fully inside an anonymous VMA that is at
   least 8 MiB or marked `MADV_HUGEPAGE`, allocate order 9 and map it with a PDE; fall back to 4 KiB. Count
   `AnonHugePages` in `/proc/meminfo`.
4. Tests: parent and child writes diverge; forking a 512 MB process doesn't double memory use; a 1 GB malloc uses
   huge pages; partial munmap of a huge page splits it; after exit the free page count returns to its baseline.

**Done when.**
- [ ] Tests pass in QEMU with `-m 4G`.

**Cost.** fork copies PTEs, never data; a COW fault copies one page (or none when refcount is 1); a huge-page fault is one order-9 allocation; a split is 512 PTE writes.

**Watch out.**
- Refcounts are atomic and the copy-or-reuse decision happens under the page-table lock.
- Splitting a huge mapping needs a TLB flush (shootdown from M1-07).

**Notes.**
- From M1-07: `mm_fork` (mm.c) copies every page under the source's `pt_lock` through `vmm_copy_range_in` (a long
  hold for a big process; COW replaces it). `zap_range` collects unmapped frames on a `page_list` through each
  page's `lru` link and frees them after the TLB flush: a COW page mapped by two mms can't sit on two lists, so the
  unmap path must drop a reference and list only pages whose count reaches 0. `MAP_SHARED | MAP_ANONYMOUS` returns
  `-EINVAL` until pages are refcounted. The fault path's "page already there" case rewrites the entry with the
  VMA's flags; with COW that must not make a shared page writable. The kernel reaches frames through the HHDM
  (`page_to_virt`); the HHDM-wide flags question is left here. COW needs CR0.WP, which the kernel never sets
  (Limine leaves it on): set it per CPU so a kernel write to a read-only user PTE faults. `walk_pt` skips a user
  PDE with PS set, so range walks ignore 2 MiB pages until this block handles them.
