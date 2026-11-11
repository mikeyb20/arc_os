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
| `port::llama` | `llama.h` | llama.cpp inference; pulls in `port::ggml`, `port::ggml_cpu`, `port::ggml_base` and pthread in link order. Only `arc-llm` links it (ADR-0012) | M2-04 |
| `ARC_LLAMA_COMPLETION`, `ARC_LLAMA_EMBEDDING`, `ARC_LLAMA_TOKENIZE` | | llama.cpp tools for test drivers; `ARC_PORTS_REF_DIR` holds the reference builds | M2-03 |

## Libraries (one home per helper, ADR-0020)

Look here and in the ports' own APIs before writing a helper. Each block that adds a library adds its row.

| Library | Header | What it does | Block |
|---|---|---|---|
| `arcjson` | `arcjson.h` | The stack's only JSON code. Parser: RFC 8259, strict, UTF-8 checked, nesting limit 64, caller's size limit, one arena per document. Writer: escaping (invalid UTF-8 becomes U+FFFD), numbers, nesting, into one growing buffer. `arcjson_utf8_complete_len` holds back a split UTF-8 character when streaming; `arcjson_utf8_valid` is the stack's UTF-8 check and `arcjson_utf8_decode` decodes one character. Manifest helpers (M3-02): `arcjson_read_file` (whole small file, size-limited), the schemas' `id` and version checks, and `arcjson_schema_version_cmp` (semver precedence, M3-06a); M5-02: `arcjson_is_schema_semver`, `_role` and `_relpath` (the schemas' rules), `arcjson_write_value` (copy a parsed value) | M2-04 |
| `arcutil` | `arcutil.h` | Shared helpers with no other home in `stack/`: SHA-256 (FIPS 180-4, streaming: `arcutil_sha256_*`), durable files (lane-l.md "Durable updates": `arcutil_write_durable` writes a temporary file, fsyncs, renames, fsyncs the directory), `arcutil_make_dir`/`arcutil_make_dirs`/`arcutil_sync_dir`, `arcutil_remove_tree` (never follows symlinks), `arcutil_ram_mib` (total RAM). Was `arcfile` (M3-06a) | M5-02 |
| `arcpack` | `arcpack.h` | The pack format's one home (`docs/formats/pack-v1.md`): reader (maps the files, checks headers and every record it reads, O(1) document, chunk and chunk-ID lookup, document text decompressed into the caller's buffer, `arcpack_plain()`) and streaming writer (checks markup and chunk ranges). The only code that runs zstd on pack content | M3-02 |
| `arcsearch` | `arcsearch.h` | Keyword search (ADR-0015): finds `/packs/<name>/<highest version>/`, opens each pack's FTS5 index read-only and immutable (skips missing or stale ones with a warning), turns a query into quoted words, merges the packs' bm25 top k. `arcsearch_build_fts` writes a pack's index (`docs/formats/pack-v1.md`) | M3-06a |

## Programs

| Program | What it is | Block |
|---|---|---|
| `arc-llm` | The model service (ADR-0012): `/v1/chat/completions`, `/v1/embeddings`, `/v1/tokenize`, `/v1/models` over HTTP/1.1 on `/var/run/arc-llm.sock`; roles from `/models/roles.json`. Parts and costs: `arc-llm/arc_llm.h` | M2-04 |
| `search` | `search [--root R] [-k N] [--pack NAME] [--] "query"`: keyword search from the command line, one result per line (score, chunk ID, title, section path). Needs no `arc-llm` | M3-06a |
| `arcpkg` | `arcpkg [--root R] [--force] install FILE \| remove NAME [VERSION] \| list \| verify [NAME] \| role [ROLE NAME VERSION]`: the package tool (ADR-0010, ADR-0012). Streams a `.arcpkg` (zstd window at most 8 MiB, ustar files and directories only) into `<install_root>/.tmp-<name>-<version>/`, hashing as it writes, then renames it to `<install_root>/<version>/` and records it in `/var/lib/arcpkg/<name>.json`. Parts and costs: `arcpkg/arcpkg.h` | M5-02 |
