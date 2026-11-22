# Article stream, version 1

Written by M3-03. The boundary between reading a source format and building a pack (content-lane.md): an
extractor turns a source (today a Kiwix ZIM, through `tools/content/zim_extract.py`) into this stream, and
`packbuild build` (`stack/packbuild/`) turns the stream into a pack's `content/` and `chunks/`
(`docs/formats/pack-v1.md`). A second source format later means a second extractor, not a second pipeline.

## Lines

JSON Lines: UTF-8, one JSON object per line, each line ended by `\n`, at most 64 MiB a line (newline
excluded). `packbuild` refuses a longer line, a last line without its newline (a file cut short), and any line
that isn't a JSON object.

**Line 1, the header:**

```json
{"stream": 1, "source": "en.wikipedia.org", "zim": "wikipedia_en_medicine_mini_2026-10.zim", "date": "2026-10-10",
 "name": "wikipedia_en_medicine", "title": "WikiMed Medical Encyclopedia", "license": "", "sha256": "c66f…0822"}
```

| Member | Meaning |
|---|---|
| `stream` | `1`, this version. `packbuild` checks it and nothing else in the header |
| `source`, `date`, `name`, `title`, `license` | the ZIM's `Source`, `Date`, `Name`, `Title` and `License` metadata (`""` when missing: no sample ZIM has `License`) |
| `zim` | the ZIM's file name |
| `sha256` | the ZIM's SHA-256, checked against `sources.json` before extraction |

**Every other line, one document:** `{"key": …, "title": …, "text": …}`, in strictly increasing byte order of
`key` (`memcmp` of the UTF-8 bytes; a repeated key is out of order). Document ID = line number − 2, so the order
is the pack's document order.

| Member | Meaning |
|---|---|
| `key` | the source's name for the document: the ZIM path, without the namespace (`Hypothermia`, `www.nhs.uk/medicines/paracetamol-for-adults/`). Not empty, no NUL. Never used as a file name |
| `title` | one line of text, no control characters, not empty (the ZIM title, else the path) |
| `text` | pack-v1 document text (`docs/formats/pack-v1.md`, "Document text"), except that a link names its target by **key**: `\u0002<key>\u001f<link text>\u0003`. An image line's reference is the image's ZIM path |

`packbuild` turns each link's key into the document ID of the line with that key (binary search over the keys
it read in its first pass); a link to a key that isn't in the stream becomes its link text. Images stay as
references until the image store (M3-08).

## How `zim_extract.py` makes it

`python3 -I tools/content/zim_extract.py --sha256 HEX [--zimcat CMD] ZIM OUT.jsonl`

1. Checks the ZIM's SHA-256 against `HEX` (from `sources.json`) and stops if they differ (content-lane.md:
   libzim's zstd frames carry no checksum).
2. Runs `zimcat stream ZIM` (below) and converts each article as it arrives, one at a time, into a temporary
   file next to `OUT`, keeping `(key, offset, length)` per article.
3. Sorts that index by key and copies the articles into `OUT.part` in key order, after the header; fsyncs and
   renames it to `OUT`. On any failure no `OUT` (and no temporary file) is left.
4. Prints a report: articles, articles without text, redirects, links, image lines, skipped entries by MIME type
   and the image types with their bytes (the M3-08 image-store question).

Links to a redirect name the redirect's final target (the map comes from `zimcat stream`), so they resolve.

**HTML conversion** (Python's `html.parser`, table-driven: the tables at the top of the script):

| HTML | Becomes |
|---|---|
| `<main>` present | only what is inside it is kept (mwoffliner and zimit pages both have one) |
| `head`, `script`, `style`, `nav`, `footer`, forms, media; classes such as `navbox`, `reference`, `mw-editsection`, `reflist`, `zim-footer`, `catlinks`, `noprint`; `hidden`, `aria-hidden`, `display:none` | dropped with everything inside |
| first `<h1>` | dropped (the page title; the document's title says it) |
| `h1`–`h6` | heading line, level 1–6; links inside become text |
| `p`, `div` and other blocks; `br` | line breaks |
| `li` | a line starting `• `; `dt`, `dd` a line each |
| outermost table's `tr` | one line, cells joined by tab; blocks and nested tables inside a cell joined by `; ` |
| `a` to a path in the ZIM | link by key; to a fragment, another site or another scheme: its text |
| `figure` with an image in the ZIM | image line: the image's path and the `figcaption` (else `alt`) as caption; an image outside the ZIM (a `data:` placeholder in nopic ZIMs) leaves the caption as text |
| other `img` | an image line after the current line; icons (both sides ≤ 32 px) dropped; mwoffliner's formula images become their `alt` formula, inline |

Whitespace collapses to one space, spaces before `, . ; : ! ? ) ]` left by dropped elements go, and control
characters are removed, so the only control bytes in `text` are pack markup, tabs between cells and newlines.

## `zimcat stream`

What `zim_extract.py` reads (`stack/zimcat/`, ADR-0022). Text lines end with `\n`; fields are tab-separated;
paths, titles and metadata print control characters and backslashes as `\xNN` (as in `zimcat ls`).

```
zim	<uuid hex>	<major>.<minor>	<entries>	<clusters>
meta <Key>	<value>                          one per metadata entry (binary ones as "<mime, N bytes>")
redirect	<path>	<target path>            each redirect of the article namespace whose final target is an HTML
                                         article there, in path order
article	<path>	<title>	<mime>	<bytes>
<exactly <bytes> bytes of HTML>
                                         (a newline after the bytes) every HTML entry of the article namespace,
                                         in cluster order
other	<mime>	<entries>	<bytes>          every other content entry of the namespace, counted by MIME type
count	redirects	<all>	<written>
count	obsolete	<n>
end	<articles>
```

The article namespace is `C`, or `A` in a file that has no `C` (before ZIM 6.1). Articles come **in cluster
order** (by cluster, then path inside it): every cluster is decompressed once; path order would decompress one
per article on WikiMed (M3-03a Notes). Memory: 8 bytes per content entry (cluster and entry number, sorted) plus
one decompressed cluster (arczim's cache cut to one slot; WikiMed maxi's largest is 174 MB). An xz cluster stops it with exit 1 naming the cluster. `end` lets the reader tell a
complete stream from one cut short.

## Costs

`zimcat stream`: 8 bytes per content entry + one decompressed cluster; each cluster decompressed once. The extractor:
the redirect map (two paths per redirect), one article, and the index (key and two integers per article); its
temporary file is about the size of the stream. `packbuild`: the keys (key bytes + 8 per document), one line and
one document.
