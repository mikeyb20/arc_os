# M3-03 — ZIM → doc store + chunks (host)

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | R | C | M3-01, M3-02, M3-03a | M3-04 |

**Goal.** Turn a Kiwix ZIM into a pack's `content/` and `chunks/` with stable chunk IDs: `zimcat` (our reader,
M3-03a) streams the articles, a host-only Python extractor turns them into an article stream, and `packbuild` (C, `stack/`, on libarcpack) cuts it into chunks and writes the
pack. Built for the medical pack from M3-01's list. Spec written by M3-00a (2026-11-11); L because it carries both
halves of the boundary (content-lane.md).

**Read first.**
- `docs/plans/content-lane.md`, `docs/plans/lane-l.md`
- `docs/formats/zim.md`, ADR-0022, `stack/lib/arczim/arczim.h` and the Notes of M3-03a (reading order, metadata)
- `docs/formats/pack-v1.md`, `stack/lib/arcpack/arcpack.h`, `stack/README.md`
- `sources.json` and the Notes of M3-01

**Build.**
1. **Article stream format**, written into `docs/formats/article-stream.md`: JSON Lines (UTF-8, one object per
   line, at most 64 MiB a line). The first line is a header `{"stream": 1, "source": …, "zim": …, "date": …}`;
   every other line one document `{"key", "title", "text"}`, in strictly increasing byte order of `key` (the ZIM
   path).
   `text` is pack-v1 markup, except that a link names its target by key (`\u0002<key>\u001f<text>\u0003`) because
   document IDs don't exist yet, and an image line's reference is the image's ZIM path.
2. **`zimcat stream ZIM`** (new command in `stack/zimcat/`, on `arczim`; Michael chose our own reader on
   2026-11-22, ADR-0022): writes the HTML entries of namespace `C` (`A` in old files) **in cluster order**, each as
   a framed record (path, title, MIME type, byte count, then the bytes), so every cluster is decompressed once;
   redirects and other entries go out as counted summary lines. Path order would decompress a cluster about once
   per article (M3-03a Notes). Memory: 8 bytes per content entry to group them by cluster, plus the cluster cache.
   **`tools/content/zim_extract.py ZIM OUT.jsonl`** (host only, `python3 -I`): runs `zimcat stream`, converts one
   article at a time, never unpacking the ZIM, and puts the stream in key order at the end (converted articles go
   to a temporary file; a sorted index of key and offset writes them out in order).
   - Takes article entries (HTML, not redirects), sorted by path. Redirects and non-HTML entries are counted, not
     written.
   - Converts HTML with the standard library's `html.parser`, table-driven: `h1`–`h6` → headings; paragraphs, list
     items and definition terms → lines (list items start with `• `); table rows → one line, cells joined by tab;
     `<a>` to another article in the ZIM → link; `<img>`/`<figure>` → image line with the `alt` or `figcaption`
     text as caption; `script`, `style`, edit links, navigation boxes and reference markers dropped. Text gets no
     control characters (they are pack markup) and no `\r`.
   - Prints a report: articles, redirects, skipped entries, and a histogram of image types and bytes (this
     answers whether the ZIM's images are WebP, which the M3-08 image store depends on).
   - Images are **not** copied into the pack yet: the image store and its on-device format are specified with
     M3-08 by M3-00. Image lines keep their reference and caption, so the pack can gain images without new chunk IDs.
3. **`stack/packbuild/`** (C, musl, static; uses arcpack, arcjson, arcutil). `packbuild build --name N --version V
   --tags a,b STREAM OUTDIR`:
   - Pass 1 reads the keys into one sorted array (keys are sorted in the stream; a key out of order or repeated
     fails the build), so document ID = line number − 1 and a link resolves by binary search.
   - Pass 2 reads each document, rewrites its links to document IDs (a link to a key that isn't in the stream
     becomes its plain text), cuts it into chunks, and adds it with `arcpack_writer_add_doc`.
   - **Chunking** (content-lane.md): words are runs of Unicode letters and digits, counted the way arcsearch splits
     a query (promote that splitter to a shared helper if both use it; ADR-0020). Target 280 words, at most 360,
     overlap 40. A heading starts a new chunk, and the chunk's section path is the headings above it. Cut at the
     end of a line if one falls in the last quarter before the target, else after a sentence end, else between
     words; never inside markup or a UTF-8 character. A section shorter than 20 words joins the next chunk of the
     same document (the section path is the first section's).
   - Every chunk carries all of the pack's tag bits (`--tags`, in `domain_tags` order).
   - `arcpack_writer_finish` writes the files; on any failure nothing is left in OUTDIR.
4. **Lane tests** in `stack/tests/packbuild/` with generated streams: links resolve and unknown links become text;
   headings give section paths; every chunk is within the word limits, starts and ends on allowed boundaries, and
   overlaps as set; a stream out of order, with a repeated key, bad markup or an over-long line is refused and
   leaves nothing behind; two builds of the same stream are byte-identical; chunk IDs round-trip through
   `arcpack_chunk_parse`. Budgets: `packbuild_bytes` (sizes) and `packbuild_rss_kib` (a 10 000-document stream,
   through `lane_rss`).
5. **Host test** for the extractor (`ctest -L host`): the test writes a tiny ZIM with `zim_fixture`
   (`stack/tests/zim/`, M3-03a; JSON spec in, ZIM out), with headings, links, a table, an image, a redirect and a
   script, spread over clusters out of path order, and checks the stream line by line. `zimcat stream` gets its own
   lane test there.
6. **Build the medical pack** from `sources.json` on the build host (content-lane.md), twice: the two `content/`
   and `chunks/` trees must be byte-identical. Record in Notes: documents, chunks, bytes of each file, words per
   chunk (min, median, max), the image histogram, and the build time. Fill in the ZIM's `sha256` in `sources.json`
   from Kiwix's `.sha256` file and check the file against it before each build (content-lane.md). A small first
   run on the WikiMed mini sample (M3-03a Notes) is the test pack Michael asked for (2026-11-22).

**Done when.**
- [x] Lane tests and the syscall check pass in `lane.yml`; the extractor's host test passes.
- [x] The medical pack is built twice with identical files, and `arcpack_open` reads it.
- [x] `docs/formats/article-stream.md` exists and `stack/README.md` lists `packbuild`.

**Cost.** `packbuild`: memory = the sorted key array (about 40 bytes per document) + one document's text and its
chunk list + the writer's buffers; link lookup O(log documents); time linear in the text. `zimcat stream`: 8 bytes
per content entry + the cluster cache; each cluster decompressed once. The extractor holds one article at a time
plus the sort index (key and offset per article); the temporary file is about the size of the stream.

**Watch out.**
- `zimcat` is the only way into a ZIM (ADR-0022); python-libzim is not used. An xz cluster stops the build with an
  error naming it; that would be a new decision, not a fallback.
- `License` metadata is missing from both samples (M3-03a Notes): fill each pack's licence from its source's
  licence page and flag the blanks, as M3-01 asked.
- mwoffliner's HTML differs between ZIM flavours and dates; keep the conversion table-driven and test it on
  fixtures, not on one real ZIM.
- ZIM paths may hold any character except control characters; JSON-escape them, and never use them as file names.
- Never parse HTML in C (ADR-0011): if a step seems to need it, it belongs in the extractor.
- No FTS index, manifest or package yet: M3-04 adds them.

**Notes.**
- **Built (2026-11-22).** `zimcat stream` (`stack/zimcat/main.c`), `tools/content/zim_extract.py`, `stack/packbuild/`
  (`packbuild_core` library + CLI), `docs/formats/article-stream.md`. Word splitter promoted from arcsearch to
  libarcpack (`arcpack_word_char`, `arcpack_next_word`; pack-v1.md "Words", which now also states the chunking
  rules); arcsearch uses it, so chunk sizes and query words can't drift apart. `arczim_mime_is_html` made public.
- **Choices beyond the spec text** (small, inside the block's scope):
  - The extractor **resolves redirects**: a link to a redirect names its final target (from `zimcat stream`'s
    `redirect` lines), so 267,399 WikiMed redirects cost no documents and their links still resolve.
  - Reference lists, navboxes, edit links and the ZIM footer are dropped; inside `<main>` only, when a page has one.
    The first `<h1>` (the page title) is dropped. Formula images become their `alt` TeX inline.
  - The extractor's **host test** runs on a stand-in for zimcat (a prepared stream), because the host CI job has no
    lane binaries (`zim_fixture` and `zimcat` need the archived zstd port). The spec's fixture test (headings,
    links, a redirect, a table, a figure, a script, three clusters out of path order) is the lane test
    `lane.zim_extract`, which runs the real `zimcat` and then `packbuild`.
  - Document ID = line number − 2 counting lines from 1 (line 1 is the header); the spec's "− 1" counted from the
    first document line.
  - `packbuild` uses musl's `realloc` → `mremap` (row added to `syscall-inventory.md`, provided by M1-07).
- **Test packs** (Michael asked, 2026-11-22), private, in `/mnt/project-files/arc_os/content/sample/test-packs/`
  with a demo manifest and FTS index from the lane helpers (M3-04 owns the real ones) so `search --root` works:

  | | WikiMed mini 2026-10 | NHS Medicines 2026-09b |
  |---|---|---|
  | articles / redirects | 72,901 / 267,399 | 1,347 / 10 |
  | articles without text | 29 (empty intros in the mini edition) | 0 |
  | extract time / build time | 2 min 30 s / 15 s | 4.5 s / 1 s |
  | chunks | 79,920 | 7,967 |
  | words per chunk min / median / max | 4 / 136 / 360 | 3 / 64 / 360 |
  | links resolved / kept as text | 694,464 / 20,404 | 3,786 / 0 |
  | docs.zst / docs.idx / strings / chunks.idx (bytes) | 46,568,208 / 2,336,928 / 1,491,346 / 2,561,536 | 1,699,576 / 47,200 / 305,941 / 259,040 |
  | packbuild peak RSS | 13 MiB | 13 MiB |

  Both built twice: byte-identical. `arcpack_test scan` reads every document and chunk of both.
- **Compression**: 81.4 MB of WikiMed mini text → 46.6 MB `docs.zst` (1.75×), because each document is its own
  level-19 frame with no dictionary (pack-v1, Michael's 2026-11-10 decision). A shared dictionary or grouped frames
  would compress better (not measured here); that is a pack-v2 question, not this block's.
- **Short chunks on NHS** (median 64 words): its pages are many short `<h2>`/`<h3>` sections, and only sections
  under 20 words join the next. Good for precise hits (search lands on "Pregnancy and paracetamol"), thin as LLM
  context; the assistant (M3-09) can take neighbouring chunks. Raising `MIN_SECTION` would change chunk IDs, so
  decide before the first release pack.
- **Licences** (Michael's check; `sources.json` `license_basis` updated): neither ZIM has `License` metadata.
  WikiMed pages' footer: "Creative Commons Attribution-Share Alike 4.0 unless otherwise noted". NHS pages' schema.org
  data: copyright holder "Crown Copyright", licence `https://developer.api.nhs.uk/terms`; M3-01 recorded OGL v3.0
  from the NHS website terms. `sha256` filled in `sources.json` for both.
- **Medical pack** (wikimed-en, `wikipedia_en_medicine_maxi_2026-10.zim`, 1,864,210,320 bytes, SHA-256 checked
  against Kiwix's `.sha256`; built in the cloud session, Michael's choice for long runs):
  - `zimcat check` ok: 476,218 entries (208,818 content, 267,400 redirects), 3,815 clusters (3,415 zstd, 400
    uncompressed, no xz), 6.69 GB of HTML; the largest cluster decompresses to 174 MB (so `zimcat stream` now
    keeps one cache slot: in cluster order a cluster is never needed twice).
  - Extract: 104,217 articles (25 without text), 2,077,156 links, 128,175 image lines, 100,773 icons dropped, 61
    images outside the ZIM; 15 min (Python's HTML parser is the bottleneck: `zimcat stream` alone takes 5 s).
    Stream 551 MB.
  - **Images** (the M3-08 question): 98,655 WebP (808 MB), 5,856 SVG (36.6 MB, mostly formula and icon images
    the extractor doesn't use as image lines), 4 PNG. So yes, WebP.
  - packbuild: 104,217 documents, 573,920 chunks, words per chunk 1 / 102 / 625 (min / median / max; one chunk
    over 360 is a single 625-word figure caption, below), 2,077,156 links resolved, 0 kept as text; 2 min, peak RSS
    16 MiB. Built twice: byte-identical. `arcpack_test scan` reads all of it back.
  - Bytes: `docs.zst` 195,907,684, `docs.idx` 3,339,040, `strings` 11,280,275, `chunks.idx` 18,369,536 (228.9 MB in
    all) for 479.7 MB of text, 2.45× (14× smaller than the HTML). The demo FTS index adds 298,676,224 bytes, more than the
    content. Against `tools/content/budget.py` (keyword-only terms): text 1,047,708,672 estimated (the nopic ZIM) vs
    228,896,535 real; FTS 523,854,336 (0.5 × nopic) vs 298,676,224; chunks 561,273 vs 573,920. The estimate errs
    about 3× large in total, so the 32 GB tier may hold more than M3-01 planned; M3-04 measures every pack and
    should feed real ratios back into `budget.py`.
  - Median 102 words per chunk: Wikipedia sections are short and a heading always starts a chunk. Fine for
    precise hits ("Snakebite > Treatment > First aid" ranks first); thin for LLM context (see NHS above).
  - **Bug found and fixed** by this build: an image line longer than 360 words (`Αr15_RNA`, a 625-word figure
    caption) made the window before it end inside the previous chunk and left an empty chunk after it, which the
    arcpack writer refused ("range outside the text"). Now the window before a long construct ends at its start
    and the construct is one chunk of its own (pack-v1.md "Words"); lane test `test_long_construct`. The two test
    packs rebuilt with the fix are byte-identical to before (they had no such line).
- **Next**: M3-04 (FTS index, manifest from `sources.json`, `.arcpkg`, tier sizing) reads `packbuild`'s output;
  Michael's licence check for WikiMed (CC BY-SA 4.0 evidence) before a release pack.
