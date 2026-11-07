---
name: port-package
description: Port an outside program or library (musl, llama.cpp, SQLite, zstd, uACPI, ...) to arc_os with archived pinned source, a patch folder, a reproducible build recipe and an .arcpkg. Use whenever third-party code is brought in.
---

# port-package

Every outside component is archived in Michael's own repos so no build needs a third-party server
(plan §3.4). Ported user-space software links musl, statically (ADR-0001, ADR-0002).

## 1. Archive and pin
- Download the exact release tarball once; record name, version, upstream URL, SHA-256 and license.
- Store it in the archive location defined in M0 (Michael-owned repo / release assets), never only upstream.
- Add an entry to `ports/<name>/PORT.md`: version, SHA-256, license, why we need it, which plan item it serves.

## 2. Layout
```
ports/<name>/
  PORT.md          version, hash, license, notes, known gaps
  build.sh         reproducible build, runs inside the dev container, no network access
  patches/         numbered *.patch files applied in order; each explains why in its header
```
Keep patches minimal. Prefer fixing arc_os (adding a missing syscall) over patching the program.

## 3. Build
- Use the pinned `x86_64-linux-musl` toolchain from the dev container; static; target `x86-64-v3`.
- The build must run with networking disabled. Fetching anything at build time is a bug.
- Kernel-side libraries (uACPI) use `x86_64-elf-gcc` and the kernel's freestanding flags instead.

## 4. Find missing syscalls
- Run the program under `strace -f` on Linux with a representative workload; diff the syscall set against
  `syscall-table.md`. Add missing ones with the `add-syscall` skill before patching around them.

## 5. Package
- Produce an `.arcpkg` (ADR-0010) with a manifest listing every file's SHA-256, license and source.

## 6. Test
- A QEMU integration test that runs the ported program's own smoke test (e.g. `llama-cli` on a tiny model,
  `sqlite3` on an FTS5 query).

## Done when
Archived source + hash recorded, offline build passes in the dev container, `.arcpkg` installs, QEMU smoke test
passes, PORT.md lists any gaps.
