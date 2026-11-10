# Lane L — conventions for the Linux-first user-space blocks

Written by M2-00a (2026-11-08). Applies to M2-01, M2-02, M2-03, M2-04, M3-02, M3-06a and M5-02 (work-blocks.md §5)
and to later user-space blocks that build on them. Every lane spec lists this file under "Read first".

## Why the lane works

arc_os implements the Linux x86_64 syscall ABI (ADR-0001) and the System V process start (ADR-0002). A static musl
program built for `x86-64-v3` therefore runs the same on Linux and on arc_os. The lane builds and debugs these
programs natively on Linux while the M1 kernel blocks are still in progress, and proves with `strace` that they ask
the kernel only for what the kernel plan provides. M2-04b then runs them in QEMU; a difference there is a kernel bug.

## Language and libraries

- In-house code is **C11** with the repo style (CLAUDE.md: K&R braces, 4 spaces, `snake_case`, header guards
  `ARCHOS_<DIR>_<FILE>_H`, a doc comment on every public function, a header comment on every `.c` file).
- Programs that embed a ported library (llama.cpp, SQLite, zstd) link **musl**, statically, because one static
  binary can't hold two C libraries. libarc stays the C library of the base system (`userland/`: init, login, shell,
  coreutils). (Michael, 2026-11-08: musl for the LLM and library stack, libarc for the OS itself; knowledge-os.md §3.4.)
- C++ appears only where a port needs it: a program that links llama.cpp is linked with `g++` (static libstdc++).
- No new outside component without Michael's OK. Small, well-known algorithms we need (JSON, SHA-256, ustar) are
  written once in `stack/lib/` (one home per helper, ADR-0020), not vendored.

## Tree

```
toolchain-musl.cmake      CMake toolchain file: x86_64-linux-musl-gcc/g++, -march=x86-64-v3, -static   (M2-01)
ports/<name>/             PORT.md, build.sh, patches/ (port-package skill)                           (M2-01…M2-03)
stack/                    CMake project for in-house musl programs and libraries                      (M2-01)
  CMakeLists.txt          lane_test() helper, label `lane`
  budgets.txt             lane budgets (sizes and counts; tools/budget-check.py --budgets)
  lib/arcjson/            JSON parser and writer                                                       (M2-04)
  lib/arcpack/            pack reader and writer (format: docs/formats/pack-v1.md)                     (M3-02)
  lib/arcsearch/          keyword retriever and the FTS index schema                                   (M3-06a)
  lib/arcutil/            SHA-256, durable file writes, directory helpers                              (M5-02)
  arc-llm/  search/  arcpkg/                                                                           (M2-04, M3-06a, M5-02)
  tests/                  lane tests and fixtures
tools/lane/build.sh       fetch archived sources, build ports, configure and build stack/ into build-lane/ (M2-01)
tools/lane/port-env.sh    compiler, flags and helpers every ports/<name>/build.sh sources                (M2-02)
ports/<name>/build-ref.sh optional test-only reference build with the host compiler, build-lane/ref/<name> (M2-03)
tools/lane/run.sh         run one arc_os-bound binary, under strace when ARC_LANE_LOGS is set           (M2-01)
.github/workflows/lane.yml                                                                             (M2-01)
```

`userland/` stays the libarc base system built by the main CMake project with the kernel toolchain. `stack/` is a
separate CMake project because it uses the other toolchain. Build outputs go to `build-lane/` (gitignored).

## Paths on arc_os and in tests

- Every lane program takes `--root DIR` (default `/`) and resolves every ADR-0009 path under it. Tests run against
  a temporary root; nothing touches the host's real `/var`, `/models` or `/packs`.
- Installed layout (pinned here because M2-04 and M3-06a read what M5-02 writes):
  - models: `/models/<name>/<version>/model.json` plus the GGUF file it names; roles in `/models/roles.json`
  - packs: `/packs/<name>/<version>/` holding `manifest.json`, `content/`, `chunks/`, `fts/`, `vectors/`
  - programs: `/apps/<name>/<version>/`
  - package database: `/var/lib/arcpkg/<name>.json`; `arc-llm` socket: `/var/run/arc-llm.sock`
  - `arcpkg` installs a package's payload into `<install_root>/<version>/`. Models keep every installed version
    (roles pick one, ADR-0012); for packs and programs a newer version replaces the older one after it is verified,
    and readers use the highest installed version if both exist for a moment.
- Durable updates: write a temp file in the same directory, `fsync`, `rename` (knowledge-os.md §3.5).

## Tests and the syscall check

- Lane tests are ctest tests in `stack/` with the label `lane`, registered with `lane_test()`. They run on Linux on
  the musl binaries we built. `ctest --test-dir build-lane/stack -L lane` is the whole lane suite.
- A test that starts an arc_os-bound binary starts it through `tools/lane/run.sh <log-name> <command…>`. Drivers
  (shell, Python, `curl`) run untraced: they never reach arc_os. In `stack/`, `lane_test()` registers a program that
  is the whole test (run through `run.sh` for you); `lane_driver_test()` registers a driver, which finds `run.sh`
  in `$ARC_LANE_RUN` and writes budget lines to `$ARC_LANE_BUDGET_DIR/<suite>.budget.log`. C lane tests report
  with `stack/tests/lane_check.h` (`lane_check()`, `lane_check_done()`), never a local copy.
- `lane.yml` sets `ARC_LANE_LOGS`, runs the suite, then
  `tools/syscall-inventory.py --check --logs $ARC_LANE_LOGS --tbl <syscall_64.tbl> --allow docs/plans/syscall-inventory.md`.
  Anything not in a table row of `syscall-inventory.md` fails, named. The fix is either code that doesn't need it,
  or a row in its "Lane additions" tables naming the M1 block that provides it (and a `syscall-table.md` row if the
  syscall has none). Never filter a log to make the check pass.
- musl reads the clock through the vDSO on Linux, so `clock_gettime` is invisible here; on arc_os it is a syscall
  (M1-06). Linux also allows things arc_os may implement later than the lane needs; M2-04b finds those.
- Sizes and counts go in `stack/budgets.txt` (ADR-0019), checked in `lane.yml` by
  `tools/budget-check.py --budgets stack/budgets.txt --log-dir build-lane/stack/budgets`. Tests write
  `ARC_TEST budget <name> <value>` lines to `<suite>.budget.log` there (the metric's source is `<suite>`); binary
  sizes come from `tools/lane/sizes.py`, peak memory from the driver (`ru_maxrss` of the child). Never time.

## Light enough for 4 GB

The low tier is 4 GB of RAM shared by the OS, a 1–1.5 GB chat model, a ~100 MB embedder and the page cache.

- Read-only data (models, packs, indexes) is memory-mapped or read in bounded pieces, never copied whole into heap.
- Streaming everywhere: package install, decompression and hashing use fixed buffers, whatever the file size.
- One request at a time in `arc-llm`; no thread per connection.
- No busy-waiting and no periodic timers (ADR-0017): block in `accept`, `read` or `poll` with a real timeout.
- No scan over all documents or chunks per query or per lookup (ADR-0019): lookups by index are O(1), search goes
  through the FTS5 index.
- Each D block's **Cost.** section names the per-request or per-query work and the bytes per record.

## Hygiene

`tools/hygiene.py` and the CI format check cover `stack/` from M2-01 (ADR-0020). Before writing a helper, look in
`stack/lib/*/` headers (the table in `stack/README.md`, kept by each block that adds a library) and in the ports'
own APIs. Ports are never edited in place: changes are numbered patches in `ports/<name>/patches/`.
