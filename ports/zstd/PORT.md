# zstd — compression for `.arcpkg` and the pack document store

| | |
|---|---|
| What | zstd 1.5.7: `libzstd.a`, `zstd.h`, `zstd_errors.h` and the `zstd` CLI, static, musl, x86-64-v3 |
| Source | `third_party/SOURCES.lock` entry `zstd` (SHA-256 `eb33e51f…6fa3`, archived release tarball) |
| Licence | BSD-3-Clause (zstd is dual-licensed BSD-3-Clause or GPL-2.0-only; we use it under BSD-3-Clause) |
| Why | ADR-0010 (`.arcpkg` = ustar + zstd), ADR-0011 (pack `content/` store), knowledge-os.md §3.4 |
| Used by | `arcpkg` (streaming decompression, M5-02), pack reader and writer (M3-02), package builders (CLI, M3-04, M5-04) |
| Built by | `build.sh <prefix>`, offline, from `tools/lane/build.sh` into `build-lane/ports/zstd/` |

## Build options

| Option | Why |
|---|---|
| `ZSTD_LEGACY_SUPPORT=0` (library and CLI) | No pre-v0.8 frame formats: nothing we read was made by them, and the decoders are dead code |
| Static library single-threaded (upstream default for `libzstd.a`) | Programs only decompress, or compress small pieces; one thread keeps memory predictable |
| CLI with threads (upstream default: `-T` works) | Package builders compress multi-GB files with `-T` |
| `HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0` | The CLI's Makefile would otherwise probe the build host's libraries; we only need `.zst` |
| `zstd-release` target | Upstream's release build of the CLI (`BACKTRACE_ENABLE=0`; musl has no `backtrace()` anyway) |
| `-O3 -march=x86-64-v3` | Upstream's optimisation level and the arc_os CPU floor (same as `toolchain-musl.cmake`) |

Upstream source is copied to `build-lane/work/zstd/` and built there; `third_party/src/zstd/` stays untouched. No
patches.

## Rules for users

- Decompress with `ZSTD_d_windowLogMax` = 23 (8 MiB window), so memory stays small whatever the input
  (`lane.zstd_lib` checks a 16 MiB-window frame is refused). Package and pack builders therefore compress with a
  window of 8 MiB or less: no `--long`, no `--ultra` levels above 19 (their window is larger).
- Use the streaming API with fixed buffers for anything that can be large (lane-l.md, "Light enough for 4 GB").

## Known gaps

- None. `lane.zstd_lib` and `lane.zstd_cli` run on Linux inside the syscall inventory; the QEMU run is M2-04b.
