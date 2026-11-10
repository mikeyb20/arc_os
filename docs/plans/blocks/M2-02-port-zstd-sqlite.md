# M2-02 — Port zstd and SQLite (FTS5)

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | L | M2-01 | M2-04, M3-02, M3-06a, M5-02 |

**Goal.** Static zstd and SQLite (with FTS5) libraries and command-line tools for arc_os, built offline from the
archive with the musl toolchain, tested on Linux inside the syscall inventory. They are the base of the pack store
(ADR-0011), the keyword index and `.arcpkg` (ADR-0010).

**Read first.**
- `docs/plans/lane-l.md`, `.claude/skills/port-package/SKILL.md`, `ports/README.md`
- `third_party/SOURCES.lock` entries `zstd` and `sqlite`, `tools/inventory/run-in-alpine.sh` (the M0-04 build
  lines), Notes of M2-01

**Build.**
1. `ports/zstd/`: `PORT.md` (version, SHA-256, license BSD-3-Clause, what we use) and `build.sh` that installs
   `lib/libzstd.a`, `include/zstd.h`, `include/zstd_errors.h` and the `zstd` CLI into `build-lane/ports/zstd/`.
   Library built with `ZSTD_LEGACY_SUPPORT=0`; the CLI with multithreading (`-T`) as upstream builds it.
2. `ports/sqlite/`: `PORT.md` and `build.sh` building the amalgamation into `libsqlite3.a`, `sqlite3.h` and the
   `sqlite3` shell, with `SQLITE_ENABLE_FTS5`, `SQLITE_OMIT_LOAD_EXTENSION` (no dynamic loading on arc_os),
   `SQLITE_THREADSAFE=1`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_DQS=0`, `SQLITE_USE_URI=1`. Each option gets one line
   of reason in `PORT.md`.
3. `tools/lane/build.sh` builds both; `toolchain-musl.cmake` finds them (`find_library` / imported targets in
   `stack/CMakeLists.txt`, defined once).
4. Lane tests in `stack/tests/ports/`:
   - `lane.zstd_lib`: a small C program compresses and decompresses a 3 MB buffer with the one-shot and the
     streaming APIs (128 KiB output buffer), and decompresses frames with `ZSTD_d_windowLogMax` set to 23. The
     streaming path is what `arcpkg` uses (M5-02).
   - `lane.zstd_cli`: `zstd -3 -T2` then `zstd -d` round-trip a file, compared with `cmp`.
   - `lane.sqlite_fts5`: create an FTS5 table, insert a few rows, `MATCH` with `bm25()` ordering, check the order;
     then reopen the file read-only as `file:…?immutable=1` and query again. Immutable read-only is how packs are
     opened (M3-06a): no locks and no journal.
   - `lane.sqlite_shell`: the `sqlite3` shell runs the M0-04 FTS script.
5. Budgets in `stack/budgets.txt`: `zstd_cli_bytes`, `sqlite3_shell_bytes`.

**Done when.**
- [x] Both ports build offline (`DL_CMD`-style: no network in `build.sh`) and the binaries are static.
- [x] The four lane tests pass and the syscall check passes in `lane.yml`.

**Watch out.**
- `immutable=1` tells SQLite the file never changes. Only use it for files nothing writes while they are open
  (installed packs). Pack building (M3-04) opens normally.
- Keep the upstream source untouched; any change is a numbered patch with its reason.
- The SQLite shell reads `/etc/passwd` and `/dev/urandom` (already in the inventory). If the library alone opens
  something new, find out why before adding a row.

**Notes.**
- 2026-11-09 (M2-02 PR): `tools/lane/port-env.sh` is the one home for port build settings (musl compiler,
  `-march=x86-64-v3`, `port_begin` copies the archived source to `build-lane/work/<name>/` and applies
  `patches/`, `port_check_static`). `tools/lane/build.sh` fetches every port's archived source first; the port
  builds themselves were checked with the network removed (`unshare -n`). Neither port needs a patch.
- `stack/CMakeLists.txt` defines `port::zstd`, `port::sqlite3`, `ARC_ZSTD_CLI` and `ARC_SQLITE_SHELL` once;
  later blocks link those, never `find_library` again.
- The M0-04 FTS5 script moved to `tests/target/fts.sql`; the inventory (`run-in-alpine.sh`) and
  `lane.sqlite_shell` both run it.
- `libzstd.a` is single-threaded (upstream default), the CLI multithreaded. Levels up to 19 keep the window at
  8 MiB or less for large inputs; 20 and up (`--ultra`) and `--long` don't, so package builders must avoid them
  (`ports/zstd/PORT.md`; M5-02 writes the rule into `schemas/README.md`).
- SQLite alone opens `/dev/urandom` once (PRNG seed) and, as root, `fchown`s new journals: both already in the
  inventory (`fchown` is M1-20's). Syscall check: 8 runs, 56 syscalls, 2 files.
- Sizes: `zstd` 1 032 537 and `sqlite3` 1 694 952 loaded bytes (stripped, static), budgets +10 %.
- Next: M2-03 (llama.cpp) adds `ports/llama.cpp/` the same way.
