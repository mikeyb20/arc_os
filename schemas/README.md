# Manifest schemas (M0-07)

JSON Schema (draft 2020-12) for the manifests that the host pipeline writes and arc_os tools read. Binding
decisions: [ADR-0010](../docs/decisions/0010-arcpkg-format.md) (`.arcpkg`),
[ADR-0011](../docs/decisions/0011-data-contract.md) (packs), [ADR-0012](../docs/decisions/0012-model-service-api.md)
(models and roles).

| Schema | Describes | Lives at |
|---|---|---|
| `arcpkg.schema.json` | `manifest.json`, first member of every `<name>-<version>.arcpkg` | inside the package; recorded in `/var/lib/arcpkg` |
| `pack.schema.json` | a knowledge pack; also the `pack` object of a pack package | `/packs/<name>/<version>/manifest.json` |
| `model.schema.json` | one GGUF model; also the `model` object of a model package | `/models/<name>/<version>/model.json` |
| `model-roles.schema.json` | which installed model fills `chat`, `embed`, `router`, `expert:<domain>` | `/models/roles.json` |
| `common.schema.json` | shared pieces: ids, semver, SHA-256, relative paths, roles | — |

Rules for every schema:

- `schema_version` is an integer starting at 1. Within a version, changes are **additive only** (new optional
  fields). Removing, renaming or retyping a field, or making one required, means version 2 and a new schema file.
- Readers ignore fields they do not know, so a manifest that uses a newer additive field still validates against
  an older copy of the same version (no `additionalProperties: false`).
- Names (package, pack, model, embedder, category, domain tag) are lowercase `[a-z0-9-]`; versions are semver.
- Paths inside a package or pack are relative, with no empty, `.` or `..` components.
- An `.arcpkg` manifest's `model` or `pack` object has the same `name` and `version` as the package. A schema
  cannot say that, so tools check it (see `tests/schemas/validate.py`, `consistency()`).

Rules for package builders (M3-04, M5-04, M5-05; `arcpkg` enforces them):

- An `.arcpkg` is a zstd-compressed POSIX ustar archive with a zstd window of **8 MiB or less**: no `--long` and no
  `--ultra` levels above 19 (levels up to 19 stay within 8 MiB, `ports/zstd/PORT.md`). `arcpkg` decompresses with
  `ZSTD_d_windowLogMax` = 23 so a multi-GB package installs on a 4 GB machine; a larger window is refused.
- `manifest.json` is the first member (at most 1 MiB), then `files/` holding exactly the files the manifest lists:
  regular files and directories only (no links, devices or pax/GNU extension headers), names within the ustar
  limits, no absolute paths and no `.` or `..` parts.
- `install_root` is `/apps/<name>`, `/models/<name>` or `/packs/<name>` for a program, model or pack (the layout
  `arc-llm` and `search` read; lane-l.md), stricter than the schema's pattern.

`examples/` holds one valid example per schema (three for `.arcpkg`: program, model, pack) and invalid ones
named for the rule they break. The values in them (model sizes, pack sizes, sources) are illustrative, not
measurements. CI runs `tests/schemas/validate.py` (host test `schemas`); run it with file arguments to check
your own manifests, named `<schema>.<anything>.json`.
