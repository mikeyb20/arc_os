# SQLite — FTS5 keyword index for packs

| | |
|---|---|
| What | SQLite 3.54.1 amalgamation: `libsqlite3.a`, `sqlite3.h` and the `sqlite3` shell, static, musl, x86-64-v3 |
| Source | `third_party/SOURCES.lock` entry `sqlite` (SHA-256 `0a9c8e34…d1f9`, archived amalgamation zip) |
| Licence | Public domain (the SQLite blessing) |
| Why | ADR-0011 (pack `fts/` index: SQLite FTS5, BM25), knowledge-os.md §3.4 |
| Used by | keyword retriever (M3-06a), pack builder (M3-04); the shell is a debugging tool |
| Built by | `build.sh <prefix>`, offline, from `tools/lane/build.sh` into `build-lane/ports/sqlite/` |

## Compile options (library and shell alike)

| Option | Why |
|---|---|
| `SQLITE_ENABLE_FTS5` | The pack keyword index (BM25 ranking, `porter unicode61 remove_diacritics 2` tokenizer) |
| `SQLITE_OMIT_LOAD_EXTENSION` | arc_os has no dynamic loading (static ELF only, ADR-0002); also drops the shell's `.load` |
| `SQLITE_THREADSAFE=1` | Serialized mode: safe if a later program shares a connection between threads; the cost is an uncontended mutex |
| `SQLITE_DEFAULT_MEMSTATUS=0` | No global memory-statistics mutex on every allocation; nothing reads the statistics |
| `SQLITE_DQS=0` | Double-quoted strings are identifiers only, never string literals: catches quoting bugs in our SQL |
| `SQLITE_USE_URI=1` | Filenames may be `file:` URIs, so packs open as `file:…?immutable=1` without extra flags |
| `-O2 -march=x86-64-v3` | The usual optimisation level and the arc_os CPU floor (same as `toolchain-musl.cmake`) |

The amalgamation is copied to `build-lane/work/sqlite/` and compiled there. No patches.

## What it asks of the kernel

- The library reads 44 bytes of `/dev/urandom` once to seed its PRNG (temporary names and similar). Already in the
  syscall inventory.
- Running as root it `fchown`s new journal files to the database's owner (`robustFchown`); arc_os runs programs as
  root at first, so `fchown` must exist (in the inventory; the kernel side is M1-20). The zstd CLI uses it too.
- The shell reads `/etc/passwd` to find `~/.sqliterc`.
- Installed packs are opened with `immutable=1`: no locks, no journal, no `fcntl` locking. Only for files nothing
  writes while they are open; pack building opens normally.

## Known gaps

- None. `lane.sqlite_fts5` and `lane.sqlite_shell` run on Linux inside the syscall inventory; the QEMU run is
  M2-04b.
