# M1-07 — Virtual memory areas, demand paging and anonymous mmap

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | K | M1-02, M1-05 | M1-08, M1-12, M1-16, M1-19 |

**Goal.** Each address space is a set of VMAs; user memory is allocated on first touch; `mmap`, `munmap`,
`mprotect`, `madvise`, `mremap` and `brk` follow Linux; bad user pointers return `-EFAULT` instead of crashing the
kernel; TLB shootdown works across CPUs (ADR-0004).

**Read first.**
- `docs/decisions/0004-page-cache-and-mmap.md`
- `kernel/mm/vmm.c`, `vmm.h`, `kernel/arch/x86_64/paging.h`, `isr.c` (page-fault path), `kernel/proc/process.c`
  (address space create, destroy, fork), `kernel/include/user_access.h`, the `brk` handler
- Notes of M1-02 and M1-05

**Build.**
1. `struct mm` per process: page-table root, VMA red-black tree keyed by start address plus a sorted list,
   sleeping read-write lock, counters (RSS, total VM), and the set of CPUs that currently have it loaded.
2. VMA: start, end, protection, flags (shared/private, anonymous/file, grows-down), file + offset (used from
   M1-12), fault operation.
3. Page-fault handler. User fault: find the VMA, check access; anonymous → map a zeroed page; stack VMA grows down;
   otherwise SIGSEGV with Linux `si_code` (`SEGV_MAPERR` / `SEGV_ACCERR`). Kernel fault on a user address: same
   resolution, and if that fails, the exception-table fixup.
4. Exception table and user-copy helpers: `copy_from_user`, `copy_to_user`, `strncpy_from_user`, `get_user`,
   `put_user`, all returning `-EFAULT` through fixups. Replace every direct user-pointer access in syscall paths.
5. TLB shootdown: `flush_tlb_range(mm, start, end)` flushes locally and sends IPIs to the other CPUs that have the
   mm loaded; kernel-mapping changes flush all CPUs.
6. Syscalls: `mmap` (`MAP_PRIVATE | MAP_ANONYMOUS`, `MAP_FIXED`, `MAP_FIXED_NOREPLACE`, `MAP_NORESERVE`,
   `MAP_POPULATE`; file mappings return `-ENODEV` until M1-12), `munmap` (splits VMAs), `mprotect`, `madvise`
   (`DONTNEED` frees; `WILLNEED`, `SEQUENTIAL`, `RANDOM`, `HUGEPAGE`, `NOHUGEPAGE` accepted as hints), `mremap`
   (`MREMAP_MAYMOVE`; musl's realloc uses it), `brk` through a heap VMA.
7. `/proc/self/maps` in Linux format; `/proc/meminfo` updated.
8. Tests (musl): malloc stress (many sizes, free, realloc); mmap/munmap/mprotect edge cases (partial unmap,
   neighbouring VMAs); SIGSEGV with the right `si_code`; `read()` into an untouched mmap buffer works; `read()`
   into an unmapped address returns `-EFAULT`; a kernel self-test where a range flush reaches every CPU that has
   the mm loaded and each one acknowledges (threads in M1-16 and reclaim in M1-14 test it for real).

**Done when.**
- [ ] Tests pass.
- [ ] No syscall path dereferences a user pointer directly (review now; SMAP enforces it from M1-19).
- [ ] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
  `arch/x86_64/`, and call the HAL through `hal_*` functions, not x86 names (work-blocks.md §7); the
  `arch_includes_outside_arch` limit in the budgets file is lowered to match (ADR-0020).

**Cost.** VMA lookup O(log n); faults take the mm lock for reading only; `struct vma` from its slab cache with a size assert; empty page-table pages freed on unmap.

**Watch out.**
- Lock order (§7): mm lock before the page-table lock; never take a fault while holding a page lock.
- Allow overcommit. When memory runs out, kill the largest process with a clear log; never panic.
- L block: a clean stopping point is after steps 1–4 with their tests passing.

**Notes.**
- From M0-02: a user page fault still halts the kernel (no SIGSEGV), and the user stack is a fixed 64 KiB
  (`USER_STACK_PAGES` 16). Growable stacks and fault-kills belong here.
- From M1-01: signal frames are built only at syscall exit, from the `SyscallFrame` (`sig_setup_frame` in
  proc/signal.c). A SIGSEGV with a handler raised by a page fault needs the same frame built from the interrupt
  frame and a return through `iretq`; without a handler, ending the process with `proc_exit` is enough. Other
  user exceptions (#UD, #GP, #DE, #XM, #MF, #DB, #AC) also still halt the kernel today.
