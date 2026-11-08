# M0-07 — Manifest schemas

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| S | R | C | — | M3-01, M3-02, M5-02 |

**Goal.** JSON Schemas for package, pack and model manifests and the model-role config, each with
`schema_version`, so the host pipeline and arc_os tools agree from day one (ADR-0010, 0011, 0012).

**Read first.**
- `docs/decisions/0010-arcpkg-format.md`, `0011-data-contract.md`, `0012-model-service-api.md`
- `docs/plans/knowledge-os.md` §6–7

**Build.**
1. `schemas/arcpkg.schema.json`: id, version, kind (`program` | `model` | `pack`), schema_version, files
   (path, size, sha256), license, source, requires (ram_mb, disk_mb, cpu_features), provides.
2. `schemas/pack.schema.json`: id, version, schema_version, category, priority, size, license, source,
   checksums, provides (fts, `vectors/<embedder>`), chunking parameters, domain tags.
3. `schemas/model.schema.json`: name, version, roles, parameter count, quantization, ram_mb, context length,
   chat template, license, sha256; embedder id and dimensions for `embed` models.
4. `schemas/model-roles.schema.json`: `chat`, `embed`, `router`, `expert:<domain>` → installed model ids
   (`/models/roles.json`, ADR-0009).
5. `schemas/examples/`: one valid and one invalid example per schema.
6. `tests/schemas/validate.py` (pip `jsonschema`), run in CI: valid examples pass, invalid ones fail.

**Done when.**
- [ ] CI validates the examples.
- [ ] ADR-0010, 0011 and 0012 link their schema.

**Watch out.**
- `schema_version` is an integer starting at 1. Within a version, changes are additive only.
- IDs are lowercase `[a-z0-9-]`; versions are semver strings.

**Notes.**
- 2026-11-08 (built). Five schemas in `schemas/` (the four above plus `common.schema.json` for shared ids, semver,
  SHA-256, relative paths and roles), `schemas/README.md`, 16 examples, `tests/schemas/validate.py` as host test
  `schemas` (python3-jsonschema added to dev-setup; works with Ubuntu's 4.10 and with 4.18+).
- Field names follow ADR-0010 where this spec differed: `name` (not `id`, also for packs; it is the pack id in
  chunk IDs), `requires.disk_bytes` (not `disk_mb`), `arch` + `cpu_level`, `install_root`, `requires.packages`.
  The spec's `cpu_features` and `provides` are kept as optional fields. knowledge-os.md §6 now says `name`.
- Kind-specific fields: a model or pack package embeds the full model or pack manifest (`model` / `pack`),
  whose name and version must equal the package's; `validate.py` checks that (a schema cannot).
- No `additionalProperties: false`: "additive only within a version" means an older reader must accept newer
  optional fields. Typos are caught by required fields and types, not by unknown-field rejection.
- `pack.provides` must include `fts` (search works without the LLM, ADR-0015). `model-roles` maps a role to a
  model name **and version**, since upgrade installs a new version beside the old one (ADR-0012).
- Invalid examples must fail for the reason in their name (`EXPECTED` in validate.py), not by accident.
