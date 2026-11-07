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

**Watch out.**
- Refcounts are atomic and the copy-or-reuse decision happens under the page-table lock.
- Splitting a huge mapping needs a TLB flush (shootdown from M1-07).

**Notes.**
-
