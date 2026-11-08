# M3-02 — `libarcpack`: doc store, chunks, manifests

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | L | M2-00a, M2-02, M2-04, M0-07 | M3-03, M3-06a, M3-07 |

**Goal.** The binary layouts of a pack's `content/` and `chunks/` (ADR-0011 leaves them to M3) and the one C
library that writes and reads them. The host pipeline (M3-03/M3-04), the retriever and the reader all use it, so
the format has a single home. Tested on Linux inside the syscall inventory; on arc_os from M3-07.

**Read first.**
- `docs/plans/lane-l.md`, `docs/decisions/0011-data-contract.md`, `0013-utf8-everywhere.md`
- `docs/plans/knowledge-os.md` §6, `schemas/pack.schema.json`, `schemas/examples/pack.valid.json`
- `stack/lib/arcjson/arcjson.h` (M2-04), `zstd.h` (M2-02), Notes of M2-04

**Build.**
1. `docs/formats/pack-v1.md`: every file, header and record, byte by byte. Fixed by ADR-0011: little-endian, 64-bit
   offsets, page-aligned arrays that can be memory-mapped, a header with magic, `schema_version` and record count,
   chunk IDs `pack:document:chunk-number`. The layout to write down unless the session finds a concrete problem
   (then it says what and why in the PR):
   - Every binary file starts with a 4096-byte header: magic (8 bytes, per file), `schema_version` u32,
     `record_size` u32, `count` u64, then zero padding; records start at offset 4096.
   - `content/docs.zst`: one independent zstd frame per document, concatenated, so any document decompresses
     alone. `content/docs.idx`: 32-byte records: frame offset u64, frame size u32, text size u32, title offset u32
     and length u32 in `content/strings`, first chunk u32, chunk count u32. Document ID = record index.
     Optional `content/dict.zstd`: a shared zstd dictionary, used when present.
   - `chunks/chunks.idx`: 32-byte records: document ID u32, byte offset u32 and length u32 of the chunk inside the
     document's text, section-path offset u32 and length u32 in `content/strings`, padding u32, domain tags u64
     (bitmask; bit meanings listed in the pack manifest). Global chunk index = record index; every index (FTS rowid,
     vector row) uses it. Chunk number inside a document = global index − the document's first chunk.
   - Document text format: UTF-8 with only the line markup the reader needs (headings, links to other documents by
     ID, image references), defined in the same file. A chunk is a byte range of that text, so text is stored once;
     `arcpack_plain()` strips the markup for indexing and embedding.
2. `stack/lib/arcpack/`: reader `arcpack_open(dir)` / `arcpack_close`, document count, title and text
   (decompressed into a caller buffer sized from the record), chunk count and record, chunk text, `arcpack_plain()`,
   chunk-ID format and parse (`pack:doc:chunk` ↔ global index, O(1) through `first_chunk`), and the manifest's
   name, version and tag names through arcjson. Writer: `arcpack_writer_open(dir, name, version)`, `add_doc(title,
   text, chunk ranges, section paths, tags)`, `finish()`; it writes `content/` and `chunks/` (the manifest is the
   pipeline's job, M3-04). `_Static_assert` on both record sizes.
3. The reader maps `docs.idx`, `chunks.idx` and `strings` read-only and validates headers on open; every offset and
   length read from a record is checked against the file size before use. A damaged pack returns an error, never
   a crash: packs come from a USB key.
4. Lane tests in `stack/tests/arcpack/`: write and read back a three-document pack (titles, text, sections, tags,
   chunk IDs both ways); `arcpack_plain()` cases; damaged packs (bad magic, wrong `schema_version`, truncated index,
   offset past the end, chunk range outside its document) each give an error; a synthetic 200 000-chunk pack where
   1000 random chunk lookups stay within the RSS budget. Run the test packs' `manifest.json` through
   `tests/schemas/validate.py`.
5. Budgets: `arcpack_lookup_rss_kib` (the 1000-lookup run). Add arcpack to `stack/README.md`.

**Done when.**
- [ ] `docs/formats/pack-v1.md` is written and ADR-0011 links it.
- [ ] Lane tests pass and the syscall check passes in `lane.yml`.

**Cost.** Chunk and document lookup O(1) by index. 32 bytes per chunk and per document record, plus the strings
file; text stored once, compressed per document. Reading a chunk decompresses its one document into a caller buffer
(no heap); nothing scales with the pack's size except the mapped files, which live in the page cache.

**Watch out.**
- u32 document IDs and in-document offsets cap a pack at 4 G documents and documents at 4 GiB of text; fine for
  every source we plan. Global chunk counts fit u32 too, but store counts as u64 in headers.
- Chunk IDs are stable only within a pack version (ADR-0011). Bookmarks (M3-07) must store the pack version too.
- Don't build FTS here. The FTS index schema belongs to the retriever (M3-06a), which uses this library to fill it.

**Notes.**
-
