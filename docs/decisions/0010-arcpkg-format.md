# ADR-0010: `.arcpkg` package format

- **Status:** Accepted
- **Date:** 2026-11-07

## Decision
- One format for programs, models and knowledge packs: `<name>-<version>.arcpkg` = a **zstd-compressed POSIX ustar**
  archive. The first member is `manifest.json`; the payload sits under `files/`.
- `manifest.json` (JSON schema in `schemas/`, versioned by `schema_version`):
  `schema_version`, `kind` (`program` | `model` | `pack`), `name`, `version`, `arch`, `cpu_level` (e.g. `x86-64-v3`),
  `requires` (`ram_mb`, `disk_bytes`, other packages), `install_root` (per ADR-0009), `license`, `source`,
  `files` (`path`, `size`, `sha256` for every file), plus kind-specific fields (model role/quantization/context/template;
  pack category/priority/provides/embedder).
- Install: extract to a temp directory on the destination filesystem, verify every SHA-256, then `rename` into place
  and record it in `/var/lib/arcpkg`.
- `arcpkg verify` re-checks installed files against the recorded hashes.

## Consequences
- One tool installs programs, upgrades models and adds data, offline from USB or disk.
- Large packs may be split into several `.arcpkg` files that depend on each other.
- Rules out: per-kind ad-hoc formats.

## Verification
- Host tests for manifest validation and hash checking; QEMU test installs, verifies and removes a package.
