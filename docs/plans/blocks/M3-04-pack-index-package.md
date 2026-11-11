# M3-04 — FTS5, tier selector, `.arcpkg` (host)

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | C | M3-03, M5-02 | M3-04b, M3-10, M3-11 |

**Goal.** Finish keyword-only packs: FTS index, `manifest.json`, the `.arcpkg`, and a tier selector that says
which packs each storage tier installs. Then build and size the whole 32 GB set. Vectors are M3-04b (after M2-06).
Spec written by M3-00a (2026-11-11), which split embeddings out of this block (content-lane.md).

**Read first.**
- `docs/plans/content-lane.md`, Notes of M3-01 and M3-03
- `docs/formats/pack-v1.md` ("Manifest", "fts/index.sqlite"), `schemas/pack.schema.json`,
  `schemas/arcpkg.schema.json`, `schemas/README.md`
- `stack/lib/arcsearch/arcsearch.h`, `stack/arcpkg/arcpkg.h`, `stack/README.md`

**Build.**
1. **`packbuild index DIR`**: `arcsearch_build_fts` on the pack.
2. **`packbuild manifest DIR --sources sources.json`** writes `manifest.json` (`pack.schema.json`) with
   `arcutil_write_durable`: name, version, title, category, priority, licence, source and domain tags from the pack's
   `sources.json` entry; chunking (`words`, 280, 360, 40); `document_count` and `chunk_count` from the files;
   `provides: ["fts"]`; `size_bytes`; and `checksums` (SHA-256 of every other file), written last because the index
   must exist first (pack-v1.md).
3. **`arcpkg build DIR -o FILE`**: a ustar writer next to the reader in `stack/arcpkg/tar.c`, and a package manifest
   made from the pack's (`kind: pack`, `install_root: /packs/<name>`, `arch: any`, `requires.disk_bytes`, the file
   list with sizes and hashes, the `pack` object = the pack manifest). zstd with `ZSTD_c_windowLog` ≤ 23, checksum
   on, a fast level (pack files are already compressed). M5-04 and M5-05 reuse it for programs and models.
4. **`tools/content/tiers.py`** (host): reads the built packs' manifests and `.arcpkg` sizes, M3-01's budget and
   the tier's models, and picks packs in `priority` order until each tier's budget is spent. Vectors count with
   M3-01's estimate (chunks × 384 bytes) until M3-04b measures them. It writes `tiers.json`: per tier, the packs
   with name, version and installed bytes, plus the budget terms. The installer (M5-03) reads this file, so its
   format is written into `schemas/tiers.schema.json` with a valid example.
5. **Lane tests** (`stack/tests/packbuild/`): index + manifest + `arcpkg build` on a generated pack; the manifest
   passes `tests/schemas/validate.py`; checksums cover every file; the package's frame header has a window
   ≤ 8 MiB and the checksum flag; `arcpkg install`, `verify` and `search` find a known chunk in it; a damaged pack
   (missing file, wrong header) is refused by `packbuild manifest`. Host test for `tiers.py` with made-up manifests:
   priority order, never over budget, a pack too big for a tier is skipped and reported.
6. **Build the 32 GB set** on the build host: every `32gb` source in `sources.json` through M3-03 and steps 1–3,
   then `tiers.py`. Record per pack: documents, chunks, text, strings, FTS and `.arcpkg` bytes, and build time;
   compare with M3-01's estimates and correct the formula's FTS ratio. Each pack installs with `arcpkg` into a
   temporary root and answers one query from its category with `search`.

**Done when.**
- [ ] Lane tests, the syscall check and the host tests pass.
- [ ] The 32 GB keyword-only set is built and sized; `tiers.json` lists the 32 GB and 1 TB tiers; its schema validates.
- [ ] `stack/README.md` lists the new `packbuild` commands and `arcpkg build`.

**Watch out.**
- Same pack version, same bytes: keep the stream and options with the pack's build record, so M3-04b can rebuild
  `content/` and `chunks/` identically and only add `vectors/`.
- `requires.packages` stays empty for these packs: vectors come inside the next pack version, not as a separate
  package (content-lane.md).
- A pack over a tier's whole budget (full Wikipedia in the 32 GB tier) is a M3-01 decision, not something the
  selector works around.
- The architecture review's search-index guards (scores 0–1, index format in the manifest, SQL only in arcsearch)
  are on hold (Michael, 2026-11-11): build to this spec only.

**Notes.**
-
