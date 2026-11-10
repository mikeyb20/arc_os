# stack/ — the musl-built user-space stack

In-house programs and libraries that embed ported code (llama.cpp, SQLite, zstd) and therefore link musl
(knowledge-os.md §3.4). Conventions: [`docs/plans/lane-l.md`](../docs/plans/lane-l.md). The base system (init,
login, shell, coreutils) is `userland/` on libarc and is not built here.

```
tools/dev-setup.sh musl                      # the pinned x86_64-linux-musl toolchain
tools/lane/build.sh                          # ports into build-lane/ports, this project into build-lane/stack
ctest --test-dir build-lane/stack -L lane    # lane tests on Linux
```

## Ports

Imported targets, defined once in `CMakeLists.txt` (`arc_port_library`, `arc_port_program`). Use the port's own API
before writing a helper.

| Target / program | Header | What it gives | Block |
|---|---|---|---|
| `port::zstd` | `zstd.h` | zstd compression, streaming decompression (cap the window at 8 MiB: `ports/zstd/PORT.md`) | M2-02 |
| `port::sqlite3` | `sqlite3.h` | SQLite with FTS5 (options: `ports/sqlite/PORT.md`) | M2-02 |
| `ARC_ZSTD_CLI`, `ARC_SQLITE_SHELL` | | paths of the `zstd` and `sqlite3` programs, for test drivers | M2-02 |

## Libraries (one home per helper, ADR-0020)

Look here and in the ports' own APIs before writing a helper. Each block that adds a library adds its row.

| Library | Header | What it does | Block |
|---|---|---|---|
