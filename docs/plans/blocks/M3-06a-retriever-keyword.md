# M3-06a — Retriever I: keyword search

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| S | D | L | M3-02 | M3-04, M3-06, M3-07 |

**Goal.** `search(query, packs, k)` → chunk IDs and scores (ADR-0011) through each pack's SQLite FTS5 index, as a
library and a `search` command that work with no `arc-llm` (ADR-0015). This block also fixes the FTS index schema
that M3-04 builds. Tested on Linux inside the syscall inventory.

**Read first.**
- `docs/plans/lane-l.md`, `docs/decisions/0011-data-contract.md`, `0015-library-without-llm.md`
- `docs/formats/pack-v1.md`, `stack/lib/arcpack/arcpack.h` (M3-02), `sqlite3.h` (M2-02), Notes of M3-02

**Build.**
1. FTS schema, written into `docs/formats/pack-v1.md`: `fts/index.sqlite` holds
   `CREATE VIRTUAL TABLE fts USING fts5(title, section, body, content='', tokenize='porter unicode61 remove_diacritics 2')`
   with rowid = global chunk index, and `meta(key TEXT PRIMARY KEY, value TEXT)` with `schema_version`, `pack_name`,
   `pack_version` and `chunk_count` (ADR-0011: every index records what it was built from). Contentless: the text
   lives once, in `content/`.
2. `stack/lib/arcsearch/`: `arcsearch_build_fts(pack_dir)` fills the index from the pack through libarcpack
   (`arcpack_plain()` text; one transaction) — tests use it now, M3-04 at scale. `arcsearch_open(root)` finds
   `/packs/<name>/<highest version>/` and opens each index read-only as `file:…?immutable=1`; a pack whose `meta`
   doesn't match its manifest version and chunk count is skipped with a warning. `arcsearch_search(query, pack
   filter, k, results)`: split the query into words (Unicode letters and digits), lowercase, drop words on a small
   built-in English stopword list, quote each remaining word, join with `OR`, rank by `bm25(fts, 5.0, 2.0, 1.0)`,
   `LIMIT k` per pack, merge the packs' lists into the top k. Results: chunk ID, score, pack, title, section.
3. `stack/search/`: `search [--root R] [-k N] [--pack NAME] "query"` prints one line per result. No snippets yet
   (M3-07 shows text through libarcpack).
4. Lane tests in `stack/tests/search/` on two small packs written with libarcpack and indexed with
   `arcsearch_build_fts`: "what is hypothermia" ranks the hypothermia chunk first; a title match beats a body match;
   results from two packs merge in score order; a stopword-only query and an empty query return no results without
   an error; FTS5 syntax in the query (`"`, `*`, `-`, `NEAR(`, `AND`) is treated as words, never as an error; a pack
   with a stale index is skipped; with `--pack` only that pack answers. A grep of the `search` strace log shows no
   `socket` or `connect` (no `arc-llm` involved).
5. Budgets: `search_rss_kib` (peak RSS of one query over both test packs). Add arcsearch to `stack/README.md`.

**Done when.**
- [x] Lane tests pass and the syscall check passes in `lane.yml`.
- [x] The FTS schema is in `docs/formats/pack-v1.md`.

**Cost.** One FTS5 query per pack: index lookups proportional to the matching postings, never to the pack's chunk
count; merge O(k × packs). No heap beyond SQLite's page cache (default size) and k results. The index is opened
immutable, so no locks or journal files.

**Watch out.**
- bm25 scores from different packs aren't strictly comparable (each pack has its own term statistics). Good enough
  to merge for now; the vector rerank (M3-06) reorders the top ~200 anyway.
- Porter stemming is English. Other languages are a later decision (flag it, don't build it).
- Contentless FTS5 can't produce `snippet()` or `highlight()`; use libarcpack text instead.

**Notes.**
- Done: FTS schema in `docs/formats/pack-v1.md`; `stack/lib/arcsearch/` (`arcsearch_build_fts`, `arcsearch_open`,
  `arcsearch_search`, `arcsearch_match_expr`); `stack/search/`; `lane.search` (two packs, 30-odd checks);
  `search_rss_kib` (measured 1828 KiB). No new syscalls: the syscall check passes as it was.
- Scores are `-bm25` (higher is better), not normalized; Michael put the review's index guards (0–1 scores,
  index format in the manifest) on Hold on 2026-11-11, so neither is built here.
- Finding for M3-06: FTS5's bm25 sums the weighted term counts before saturating, so with the title weighted 5 a
  title match dominates and the chunks of one document score almost the same whatever their body says (the
  test checks the document's chunks come first, not their order). The vector rerank decides the order inside a
  document.
- Tiny packs: a word in half the chunks or more has an IDF of about 0 and scores 0; the test packs carry filler
  documents for that. Real packs have thousands of chunks.
- Reuse and promotions: `arcjson_utf8_decode` (was static), `arcjson_schema_version_cmp` (new, semver
  precedence; M5-02's arcpkg should use it), `arcfile` (`arcfile_make_dir`, `arcfile_sync_dir`, moved from the
  pack writer). `stack/tests/lane_pack.py` (manifest writer) is shared by the arcpack and search drivers;
  `stack/tests/lane_rss.c` measures a program's own peak RSS.
- `search` takes `--` before a query that starts with '-'. A query is cut at 4096 bytes of MATCH expression.
- No `/packs` directory is not an error: no packs, one warning.
