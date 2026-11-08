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
- [ ] Both ports build offline (`DL_CMD`-style: no network in `build.sh`) and the binaries are static.
- [ ] The four lane tests pass and the syscall check passes in `lane.yml`.

**Watch out.**
- `immutable=1` tells SQLite the file never changes. Only use it for files nothing writes while they are open
  (installed packs). Pack building (M3-04) opens normally.
- Keep the upstream source untouched; any change is a numbered patch with its reason.
- The SQLite shell reads `/etc/passwd` and `/dev/urandom` (already in the inventory). If the library alone opens
  something new, find out why before adding a row.

**Notes.**
-
