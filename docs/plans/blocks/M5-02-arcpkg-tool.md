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
- [ ] Lane tests pass and the syscall check passes in `lane.yml`.
- [ ] `stack/README.md` lists arcutil.

**Watch out.**
- Package builders (M3-04, M5-04, M5-05) must compress with a window of 8 MiB or less (no `--long`). Write that rule
  in `schemas/README.md` next to the format.
- `rename` of a directory onto an existing one fails; that is why each version gets its own directory.
- Disk space isn't checked up front (no `statfs` in the inventory). `ENOSPC` while extracting removes the temp
  directory and reports clearly.
- No network code at all. "Offline from USB or disk" means a file path.

**Notes.**
-
