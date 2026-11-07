# M1-17 — Threads II: full futex, per-thread signals, affinity

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | K | M1-06, M1-16 | M1-20, M1-22 |

**Goal.** Everything musl's pthreads and libstdc++'s `std::thread` need: timed waits, requeue, signals aimed at
threads, per-thread masks, CPU affinity and the CPU description files llama.cpp reads.

**Read first.**
- The futex, signal and sched rows of `docs/plans/syscall-inventory.md` (including files read under `/proc` and
  `/sys`), Notes of M1-06 and M1-16, `kernel/proc/signal.c`

**Build.**
1. `futex`: `FUTEX_WAIT` with relative timeout (monotonic), `FUTEX_WAIT_BITSET`/`FUTEX_WAKE_BITSET` (absolute
   timeout, `FUTEX_CLOCK_REALTIME`), `FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`, and `FUTEX_WAKE_OP` if the inventory
   lists it; `-EAGAIN`, `-ETIMEDOUT`, `-EINTR` exactly as Linux.
2. Signals: per-thread pending set plus process-wide pending set; delivery picks a thread that doesn't block the
   signal; `rt_sigprocmask` per thread; `sigaltstack`; `SA_RESTART` restarts, otherwise `-EINTR`; fatal signals end
   the whole group; `rt_sigtimedwait` if the inventory lists it.
3. `sched_setaffinity`/`sched_getaffinity` (return value = kernel mask size in bytes, as Linux), `sched_yield`,
   `getcpu`.
4. `/proc/cpuinfo` in Linux format and the `/sys/devices/system/cpu` files the inventory shows (at least `online`
   and `possible`).
5. Tests: condition-variable timed waits, rwlocks, barriers, `pthread_once`, keys; `tgkill` to one thread; masks
   block delivery; `read()` on a pipe restarts with `SA_RESTART`; pinning to CPU 2 is honoured (`getcpu`);
   `std::thread` waits for M2-01.

**Done when.**
- [ ] Tests pass.

**Watch out.**
- Futex waits must recheck the value under the hash-bucket lock, or wakeups get lost.
- Timeouts use the per-CPU timer queues from M1-06; no polling loops.

**Notes.**
-
