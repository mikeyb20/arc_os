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
- [ ] Lane tests and the syscall check pass in `lane.yml`; the extractor's host test passes.
- [ ] The medical pack is built twice with identical files, and `arcpack_open` reads it.
- [ ] `docs/formats/article-stream.md` exists and `stack/README.md` lists `packbuild`.

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
-
