# M0-10 — Free reaped processes

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| S | D | K | M0-09 | — |

**Goal.** A process that has exited and been reaped gives back everything it held, so the M0-08 leak budgets
(`pages_leaked`, `heap_leaked_bytes`) drop to 0. Added 2026-11-08 with Michael's OK, after M0-08's leak check
found that every exited process leaks about 54 pages and 40 KB of kernel heap.

**Read first.**
- M0-08's Notes and `tests/budgets.txt`
- `kernel/proc/process.c` (`proc_reap`, `proc_fork`, `proc_list`), `kernel/proc/proc_syscalls.c` (`sys_exit`,
  `sys_wait4`), `kernel/proc/signal.c` (`sig_terminate`), `kernel/proc/thread.c` (`thread_destroy`),
  `kernel/mm/vmm.h` (`vmm_free_user_pages`, `vmm_destroy_user_pml4`)

**Build.**
1. Today `proc_reap()` only marks the child `PROC_TERMINATED`. Make reaping free: the user pages and page tables,
   the fd table, the main thread (kernel stack, FPU area, TCB) and the `Process` itself, and unlink it from
   `proc_list`.
2. A thread never frees its own kernel stack: the parent frees it in `wait4`, after the child has switched away
   for the last time (it is a zombie and off every run queue). Check that no wait queue, signal or timer still
   points at the process or thread.
3. Orphans: children of an exiting process are re-parented to init (Linux), so init reaps them and they are freed
   too. If re-parenting already exists, test it; otherwise add it.
4. Lower `pages_leaked` and `heap_leaked_bytes` in `tests/budgets.txt` to 0.
5. Tests: host tests for reap freeing everything (kfree/pmm counters) and for unlink from the list; QEMU: the
   existing suites now report 0 for both leak budgets; a test program that forks and reaps 200 children in a loop.

**Done when.**
- [ ] `pages_leaked` and `heap_leaked_bytes` are 0 on every QEMU suite, and their limits are 0.

**Cost.** Reaping walks the child's page tables once (proportional to what it mapped) and frees O(1) kernel
objects; `proc_list` gets shorter, which helps every scan that still walks it.

**Watch out.**
- Freeing the address space while the child's CR3 is still loaded on some CPU. Single CPU today, but write it so
  M1-05 can keep it (the reaper runs on the parent, with its own CR3).
- PID reuse: a freed PID must not match a stale reference (process group, fg pgid in the tty).

**Notes.**
-
