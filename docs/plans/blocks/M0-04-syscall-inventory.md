# M0-04 — Syscall inventory

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| S | R | U | M0-03 | M0-05, M1-20 |

**Goal.** The exact syscalls, flags, ioctls and files our target programs use under musl, so M0 and M1 build
exactly what is needed and nothing else.

**Read first.**
- `docs/decisions/0001-linux-syscall-abi.md`
- `.claude/skills/arc-os-architecture/reference/syscall-table.md`

**Build.**
1. Target test programs in `tests/target/` (they become QEMU tests later, so write them once):
   `libc_basic.c` (stdio, files, directories, environment, signals, time), `pthread_basic.c`,
   `cxx_basic.cpp` (exceptions, `std::thread`, `std::filesystem`, iostream).
2. `.github/workflows/inventory.yml` (manual): in an Alpine (musl) container, build statically the three test
   programs, the SQLite shell with FTS5, the zstd CLI, and llama.cpp (`llama-cli` or `llama-simple`, plus
   `llama-embedding`) from archived sources. Run each under `strace -f -qq -o …` with the archived tiny models
   (llama.cpp with 4 threads). Upload the logs.
3. `tools/syscall-inventory.py`: merges the logs into one table: syscall, number (from the archived
   `syscall_64.tbl`), programs, flags / ioctl requests / fcntl commands / socket families seen, files opened
   (`/proc/...`, `/sys/...`, `/dev/...`, `/etc/...`).
4. Write `docs/plans/syscall-inventory.md` (generated table + a 10-line summary) and add or adjust rows in
   `syscall-table.md` (Needed by, milestone).
5. Check every number in ADR-0001's migration table against the archived `syscall_64.tbl`. Fix wrong numbers in
   the ADR as a factual correction and say so in the PR.

**Done when.**
- [ ] The inventory doc is committed and `syscall-table.md` has a row for every syscall seen.
- [ ] ADR-0001's table is verified against the archived table.

**Watch out.**
- Use the musl builds. glibc programs make different syscalls (for example `openat` everywhere, `rseq`).
- This block also proves that llama.cpp, SQLite and the C++ runtime link statically against musl. Record any
  build trouble in Notes; it is early warning for M2.

**Notes.**
-
