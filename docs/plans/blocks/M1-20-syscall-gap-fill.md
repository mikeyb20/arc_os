# M1-20 — Syscall gap fill

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K | M1-11, M1-17, M1-18 | M1-22, M2-02 |

**Goal.** Every syscall the inventory lists for musl, libstdc++, SQLite, zstd and llama.cpp is implemented, or
deliberately returns `-ENOSYS` with the reason written in `syscall-table.md`.

**Read first.**
- `docs/plans/syscall-inventory.md`, `.claude/skills/arc-os-architecture/reference/syscall-table.md`,
  `.claude/skills/add-syscall/SKILL.md`

**Build.**
1. List the rows still `missing` or `stub`. For each, follow the add-syscall skill (verify the number, uapi struct,
   handler in its subsystem, libarc wrapper only if our own programs need it, tests). Expected (confirm against the
   inventory): `getrandom` plus `/dev/urandom` and `/dev/random` (RDRAND/RDSEED mixed with TSC jitter; also the
   `AT_RANDOM` source), `uname`, `sysinfo`, `prlimit64`/`getrlimit`/`setrlimit`, `getrusage`, `access`/`faccessat`,
   `readlink`/`readlinkat` (for `/proc/self/exe`), `fchmod`/`fchown`, `link`/`symlink`, `fadvise64`,
   `set_robust_list` (store it), `prctl(PR_SET_NAME)`, `membarrier` and `rseq` (`-ENOSYS` unless the inventory
   shows musl or libstdc++ needs them), `eventfd2`/`timerfd_*` only if listed.
2. The `/proc` and `/sys` files the inventory shows (for example `/proc/self/exe`, `/proc/meminfo`,
   `/sys/devices/system/cpu/online`).
3. `syscall-table.md`: every inventory row is `done` or `-ENOSYS` with a reason.
4. A QEMU test per program class runs the musl C test programs from M0-04 end to end.

**Done when.**
- [ ] No inventory row is left without a status and a reason.
- [ ] Tests pass.

**Watch out.**
- Stay inside the inventory. A syscall nobody calls is scope creep.

**Notes.**
-
