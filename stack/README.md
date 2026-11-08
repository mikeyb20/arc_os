# stack/ — the musl-built user-space stack

In-house programs and libraries that embed ported code (llama.cpp, SQLite, zstd) and therefore link musl
(knowledge-os.md §3.4). Conventions: [`docs/plans/lane-l.md`](../docs/plans/lane-l.md). The base system (init,
login, shell, coreutils) is `userland/` on libarc and is not built here.

```
tools/dev-setup.sh musl                      # the pinned x86_64-linux-musl toolchain
tools/lane/build.sh                          # ports into build-lane/ports, this project into build-lane/stack
ctest --test-dir build-lane/stack -L lane    # lane tests on Linux
```

## Libraries (one home per helper, ADR-0020)

Look here and in the ports' own APIs before writing a helper. Each block that adds a library adds its row.

| Library | Header | What it does | Block |
|---|---|---|---|
