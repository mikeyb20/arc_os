# M5-02 — `arcpkg` tool

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | L | M2-00a, M2-02, M2-04, M0-07 | M5-03, M5-05 |

**Goal.** One offline tool that installs, removes, lists and verifies `.arcpkg` packages and switches model roles
(ADR-0010, ADR-0012), streaming so a multi-GB package installs on a 4 GB machine. Tested on Linux in a temporary
root; on arc_os from M5-03.

**Read first.**
- `docs/plans/lane-l.md`, `docs/decisions/0010-arcpkg-format.md`, `0009-directory-layout.md`,
  `0012-model-service-api.md`
- `schemas/README.md`, `schemas/arcpkg.schema.json`, `schemas/model-roles.schema.json`, `tests/schemas/validate.py`
- `stack/lib/arcjson/arcjson.h` (M2-04), `zstd.h` (M2-02), Notes of M2-04

**Build.**
1. `stack/lib/arcutil/`: SHA-256 (FIPS 180-4, streaming), durable write (temp file in the same directory, `fsync`,
   `rename`, `fsync` of the directory), recursive directory removal. Lane test with the FIPS test vectors. Add it to
   `stack/README.md`; nothing else in `stack/` may carry its own copy.
2. `stack/arcpkg/`: a ustar reader (regular files and directories only; any other member type, absolute path, `..`
   component or name over the ustar limits is an error) on top of zstd streaming decompression with
   `ZSTD_d_windowLogMax` = 23 (8 MiB window), so memory stays small whatever the package size.
3. Commands, all taking `--root R`:
   - `install FILE`: the first member must be `manifest.json` (≤ 1 MiB); check it against the schema rules a reader
     must enforce (required fields, kind, name and version format, `install_root` pattern, every file listed once,
     `model`/`pack` name and version equal to the package's, `requires.packages` installed). Models: refuse if
     `ram_mb` + 1024 MB reserve > RAM from `sysinfo` (`--force` overrides, with a warning). Extract into
     `<install_root>/.tmp-<name>-<version>/` on the same filesystem, hashing while writing; each member must match
     its manifest entry's size and SHA-256, and no member may be missing or extra. Then `fsync`, `rename` to
     `<install_root>/<version>/`, and record `/var/lib/arcpkg/<name>.json` (versions, files, hashes) with a durable
     write. For programs and packs, remove older versions after that; models keep them (lane-l.md).
   - `remove NAME [VERSION]`: refuses a model version that a role in `roles.json` points at (`--force` overrides).
   - `list`: name, kind, versions, size.
   - `verify [NAME]`: re-hash installed files against the database; name every mismatch; exit 1 if any. (Repair from
     the key is M5-05.)
   - `role` shows `roles.json`; `role ROLE NAME VERSION` points a role at an installed model version whose
     `model.json` lists that role, written durably (`arc-llm` notices by mtime, M2-04).
   - At start, leftover `.tmp-*` directories from an interrupted install are removed.
4. Lane tests in `stack/tests/arcpkg/`: the driver (Python `tarfile` in ustar format plus the ported `zstd` CLI,
   both untraced) builds fixture packages: a program, a model (tiny GGUF stand-in) and a pack. Install all three and
   `verify`; the installed manifests pass `tests/schemas/validate.py`; a wrong hash, a missing file, an extra file,
   `../` in a path and a symlink member are each refused and leave no trace (no temp directory, database unchanged);
   a model larger than RAM is refused; two versions of a model installed side by side, `role` switches between them,
   `remove` of the role's version is refused; reinstalling the same version is a no-op; a corrupted installed file
   fails `verify` by name; a 200 MB package installs within the RSS budget.
5. Budgets: `arcpkg_bytes`, `arcpkg_install_rss_kib` (the 200 MB install).

**Done when.**
- [x] Lane tests pass and the syscall check passes in `lane.yml`.
- [x] `stack/README.md` lists arcutil.

**Cost.** Install streams: memory is the zstd window (at most 8 MiB), its 128 KiB input and output buffers, one
64 KiB copy buffer and the manifest (at most 1 MiB) with its parse, whatever the package size; each file is
hashed once while written and looked up in the sorted manifest in O(log n). Disk: the package's files once, in
the temporary directory renamed into place (no copy). fsyncs: one per file, one per directory, then the
database's durable write. `verify` reads every installed file once. `list` and `verify` read one database file
per package.

**Watch out.**
- Package builders (M3-04, M5-04, M5-05) must compress with a window of 8 MiB or less (no `--long`). Write that rule
  in `schemas/README.md` next to the format.
- `rename` of a directory onto an existing one fails; that is why each version gets its own directory.
- Disk space isn't checked up front (no `statfs` in the inventory). `ENOSPC` while extracting removes the temp
  directory and reports clearly.
- No network code at all. "Offline from USB or disk" means a file path.

**Notes.**
- Done: `stack/lib/arcutil/` (was `arcfile`, M3-06a; adds SHA-256, `arcutil_write_durable`, `arcutil_make_dirs`,
  `arcutil_remove_tree`, `arcutil_ram_mib`); `stack/arcpkg/` (`tar.c` ustar over zstd streaming, `manifest.c`,
  `db.c` database and roles, `install.c`, `main.c`); lane tests `lane.arcutil` (FIPS vectors) and
  `lane.arcpkg_{install,refuse,models,big}`; budgets `arcpkg_bytes` and `arcpkg_install_rss_kib`; the builder
  rules in `schemas/README.md`. No new syscalls: the syscall check passes as it was.
- Database entry `/var/lib/arcpkg/<name>.json`: `{"schema_version": 1, "name", "versions": [{"version",
  "manifest"}]}`, lowest version first, each with the full package manifest it was installed from (files, sizes,
  hashes), at most 64 versions. `verify`, `list` and `remove` read only that, and re-check it as an install would
  before touching a path.
- `install_root` must be exactly `/<apps|models|packs>/<name>`, stricter than the schema's pattern: that is the
  layout `arc-llm` and `search` read (lane-l.md), and it lets the start-up cleanup find `.tmp-*` directories at a
  fixed depth. Written in `schemas/README.md`; widen it only with the readers.
- Programs and packs: installing a version older than the installed one is refused ("remove it first"), so
  "newer replaces older" never leaves two. Models keep every version.
- Model RAM is the larger of `requires.ram_mb` and `model.ram_mb`, plus the 1024 MiB reserve.
- A failed install removes its temporary directory and any `<area>` or `<area>/<name>` directory it created, so
  the root is byte-for-byte as before (the lane test compares snapshots). A version directory left by a crash
  between the rename and the database write is replaced by the next install of that version.
- Not tested: `ENOSPC` (no small filesystem in the lane without mount rights). The write error path is the same
  one the other refusals take: the temporary directory is removed and the error names the file and "No space left
  on device".
- One arcpkg at a time: there is no lock (no `flock` in the inventory yet). M5-03 runs it from the installer only;
  a lock belongs with the first concurrent user.
- Reuse and promotions: `arcjson_is_schema_semver`, `_role`, `_relpath` and `arcjson_write_value` (new);
  `arc-llm` now uses `arcjson_is_schema_role`, `arcjson_is_schema_relpath` and `arcutil_ram_mib` instead of its own
  copies (`tidy:` commit).
