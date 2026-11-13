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
- [x] Tests pass.
- [x] No syscall path dereferences a user pointer directly (review now; SMAP enforces it from M1-19).
- [x] Files this block rewrites outside `kernel/arch/` include `arch/hal.h` or arch-neutral headers, not
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
- From M1-03: #GP is a "paranoid" vector (`isr_paranoid`, GS decided by MSR_GS_BASE). A #GP whose RIP is
  `syscall_iret_user` (syscall_entry.asm) is the IRET back to a non-canonical user RIP (set by rt_sigreturn): treat it
  as the user's fault (SIGSEGV), not the kernel's. The syscall frame is found from `thread_current()->kernel_stack_top`
  (syscall.c `syscall_frame`); the old `syscall_saved_user_*` globals are gone.
- From M1-05: mm work runs under the interim big kernel lock (`kernel/proc/kernel_lock.h`), held for the whole
  syscall; this block gives the mm its own locks at the levels in `docs/kernel/locking.md` (mm lock 20, page-table
  lock 50; the kernel page-table lock in `vmm.c` is already level 50) and moves mm off the kernel-lock list there.
  User page faults still panic; the fault path this block writes must take the mm lock, not the kernel lock. CR3 is compared with what each CPU has loaded (`rq->cr3` in
  sched.c), so a migrated single-threaded process reloads it; shootdown is needed once threads share an mm (M1-16).
- 2026-11-13 (built). `struct Mm` (mm/mm.h, mm.c) with a VMA set (rbtree + list, vma.c), `RwSem` mm lock and
  per-mm `pt_lock`; demand-zero faults, stack growth to 8 MiB with a 1 MiB guard gap, OOM kill of the largest
  RSS (reaps another victim at once, never panics; fault.c). User copies go through one routine with an
  exception-table entry (`arch_copy_user`, uaccess.asm); fs/net/proc syscalls use bounce buffers. TLB
  shootdown: per-CPU request slot + sender mask, IPI only to `Mm.cpus` (tlb.c). CPU exceptions become Linux
  signals and signals are delivered on interrupt return (isr.c); `SyscallFrame` gained `ret_rcx`/`ret_r11`.
- Decisions: the address space is freed at exit (`proc_exit`), not at reap; exec builds a new mm and drops the
  old. ELF segments are one VMA each; `.bss` pages come by fault. mprotect over a hole fails whole (-ENOMEM).
- Surprises: musl's `sbrk(n)` refuses n != 0, `mprotect()` rounds the address, and `siglongjmp` needs
  `rt_sigprocmask` (M1-17), so `mmtest` uses raw brk and `SA_NODEFER`. A kernel fault under a spinlock skips
  resolution and fails the copy; copy user data before taking any lock.
- Next blocks: M1-08 owns COW (zap lists use `lru`, see its Notes), M1-16 shares `Mm` between threads, M1-12
  adds file VMAs (`VmaOps.fault`). Open: `vma_find_gap` is O(VMAs passed) when the hint misses.
