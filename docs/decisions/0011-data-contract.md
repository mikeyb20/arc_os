# ADR-0011: Knowledge pack data contract

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
LLMs must iterate the library fast, and future embedders, indexes or MoE expert models must plug in without
reprocessing content.

## Decision
- Pack layout: `manifest.json` (schema [`schemas/pack.schema.json`](../../schemas/pack.schema.json)), `content/` (zstd document store keyed by document ID; maps as pre-rendered tiles),
  `chunks/`, `fts/` (SQLite FTS5), `vectors/<embedder-id>/`.
- **Chunk ID** = `pack:document:chunk-number`, stable within a pack version. Every derived index records the pack
  version and the chunk count it was built from.
- Chunks are ~300–500 tokens of plain UTF-8 text. Each chunk record holds document ID, chunk number, title/section
  reference, domain-tag bitmask and its location in the source document.
- Binary arrays are little-endian, use 64-bit offsets, are page-aligned so they can be memory-mapped, and carry a
  header with magic, `schema_version` and record count. The exact record layouts are defined in M3 under this ADR:
  [`docs/formats/pack-v1.md`](../formats/pack-v1.md) (M3-02).
- Vectors are stored int8 or fp16, one row per chunk, **in chunk order**; several embedders can coexist.
- Retrieval contract: `search(query, packs, k)` returns chunk IDs and scores. FTS5 finds candidates; vectors rerank
  the top ~200. A vector index may be added later behind the same call.
- Heavy processing (extraction, chunking, embedding) happens on a Linux build host; the computer only reads packs.

## Consequences
- Adding an embedder adds a `vectors/` folder; adding an index adds a folder; content is untouched.
- Domain tags let a router send questions to an expert model.
- Rules out: indexes keyed by anything other than chunk IDs; parsing source formats (ZIM, HTML) at question time.

## Verification
- Host tests for record layouts; M3 done-test answers cited questions from the 32 GB pack set.
