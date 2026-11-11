# Knowledge pack format, version 1

Defined by M3-02 under [ADR-0011](../decisions/0011-data-contract.md). The one implementation is
`stack/lib/arcpack/` (`arcpack.h`): the host pipeline (M3-03, M3-04), the retriever (M3-06a) and the reader
(M3-07) all go through it, so this file and that library change together. Later blocks add their parts here
(M3-06a: the FTS index; M3-05: vectors).

```
/packs/<name>/<version>/
  manifest.json        schemas/pack.schema.json, written by the pipeline (M3-04)
  content/docs.zst     the documents: one zstd frame each
  content/docs.idx     one 32-byte record per document
  content/strings      titles and section paths
  chunks/chunks.idx    one 32-byte record per chunk
  fts/  vectors/       indexes (M3-06a, M3-05)
```

## Conventions

- Little-endian. Integers are unsigned. `u32` and `u64` sizes are as named.
- Every binary file starts with a **4096-byte header**. Records or data start at byte 4096, so each array is
  page-aligned and can be memory-mapped as it is.
- A file is exactly `4096 + count × record_size` bytes (`content/docs.zst`: the frames follow the header, see
  below). A reader refuses a file of any other size, and checks every offset and length it reads from a record
  against the file it points into before using it: packs come from a USB key and may be damaged.
- **Document ID** = index of the document's record in `content/docs.idx`. **Global chunk index** = index of the
  chunk's record in `chunks/chunks.idx`; every index (FTS rowid, vector row) is keyed by it.
- **Chunk ID** = `<pack name>:<document ID>:<chunk number>` in decimal, no leading zeros, where chunk number =
  global chunk index − the document's `first_chunk` (ADR-0011). It is stable only within a pack version.
- Limits that follow from `u32` fields: 2³² − 1 documents and chunks per pack, 4 GiB of text per document,
  4 GiB of strings per pack.

## Header (every binary file)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | `magic` | per file, below |
| 8 | 4 | `schema_version` | `1` |
| 12 | 4 | `record_size` | bytes per record; `content/strings`: 1; `content/docs.zst`: 0 |
| 16 | 8 | `count` | records; `content/strings`: data bytes; `content/docs.zst`: frames (= documents) |
| 24 | 64 | `pack_name` | the manifest's `name`, padded with NUL bytes |
| 88 | 64 | `pack_version` | the manifest's `version`, padded with NUL bytes |
| 152 | 3944 | | zero |

`pack_name` and `pack_version` tie every file to one build of one pack: a reader refuses a pack whose files
or manifest disagree (for example `chunks/` left over from another version). They are also what derived
indexes record they were built from (ADR-0011).

| File | `magic` (8 bytes) |
|---|---|
| `content/docs.idx` | `ARCPDOCS` |
| `content/strings` | `ARCPSTRS` |
| `chunks/chunks.idx` | `ARCPCHNK` |
| `content/docs.zst` | `5A 2A 4D 18 F8 0F 00 00` |

The `content/docs.zst` magic is a zstd **skippable frame** header (magic `0x184D2A5A`, length 4088), so the whole
4096-byte header is a skippable frame and the file stays a valid zstd stream: `zstd -d` decompresses it to all
documents' text, one after another.

## `content/docs.zst`

After the header: one zstd frame per document, in document order, nothing between them. Each frame decompresses
on its own. Frames are plain, so a future in-house decoder can stay small (Michael, 2026-11-10: no dictionary):

- single frame per document, content size in the frame header, content checksum on;
- window at most 8 MiB (`windowLog` ≤ 23), so a streaming decoder needs at most 8 MiB (the pack reader
  decompresses a whole document into a buffer of `text_size` and needs no window);
- no dictionaries, no long-distance matching, no `--ultra` levels. (The M3-02 spec allowed an optional shared
  dictionary, `content/dict.zstd`; Michael chose to leave it out.)

The writer compresses at level 19. The reader enforces the rules from each frame header: it refuses a frame
with the checksum flag off, a dictionary ID, or a window over 8 MiB. Only
libarcpack calls zstd on pack content, so an in-house decoder can later replace it without changing the format.

## `content/docs.idx`: document records, 32 bytes

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 8 | `frame_offset` | byte offset of the document's frame in `content/docs.zst` (≥ 4096) |
| 8 | 4 | `frame_size` | bytes of the frame |
| 12 | 4 | `text_size` | bytes of the decompressed text; equals the frame's content size |
| 16 | 4 | `title_offset` | offset of the title in the data of `content/strings` (after its header) |
| 20 | 4 | `title_length` | bytes of the title |
| 24 | 4 | `first_chunk` | global index of the document's first chunk |
| 28 | 4 | `chunk_count` | chunks of this document: global indexes `first_chunk` … `first_chunk + chunk_count − 1` |

A document's chunks are contiguous, and documents appear in chunk order: document *d*'s chunks follow document
*d − 1*'s. A document may have no chunks (`chunk_count` 0, `first_chunk` = the next chunk's index).

## `chunks/chunks.idx`: chunk records, 32 bytes

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `document` | document ID |
| 4 | 4 | `offset` | byte offset of the chunk in the document's text |
| 8 | 4 | `length` | bytes of the chunk (> 0) |
| 12 | 4 | `section_offset` | offset of the chunk's section path in the data of `content/strings` |
| 16 | 4 | `section_length` | bytes of the section path; 0 = the top of the document |
| 20 | 4 | | zero |
| 24 | 8 | `tags` | domain-tag bitmask: bit *i* is the manifest's `domain_tags[i]` |

A chunk is a byte range of its document's text, so text is stored once. Chunks may overlap (the manifest's
`chunking.overlap_tokens`). A chunk starts and ends on a UTF-8 character boundary and never inside a markup
construct (below), so its text can be shown or indexed on its own.

## `content/strings`

After the header: UTF-8 strings packed back to back, no separators or terminators; records point into it with
an offset (from the end of the header) and a length. Strings are shared: consecutive chunks with the same section
path point at one copy.

- **Title**: one line of text, no control characters, not empty.
- **Section path**: the headings from the top of the document down to the chunk's section, outermost first,
  separated by U+001F (unit separator); no other control characters, no empty headings. Empty for text before the
  first heading.

## Document text

UTF-8 (ADR-0013), lines end with `\n` (no `\r`). "Control character" in this file means U+0000–U+001F and
U+007F. The only ones in document text are tab, newline and these five markup bytes; the writer refuses text with any other control character or with markup used any other way:

| Byte | Use |
|---|---|
| `01` | **Heading**: at the start of a line, followed by the level `1`…`6`, then the heading text to the end of the line |
| `02` … `1F` … `03` | **Link** to a document of the same pack: `02`, the document ID in decimal (no leading zeros), `1F`, the link text (not empty), `03`; inside an ordinary line |
| `04` … `1F` | **Image**: at the start of a line, `04`, the image reference (not empty), `1F`, the caption (may be empty) to the end of the line. How a reference resolves to an image file is defined with the image store (M3-03/M3-08) |

Headings, link texts, image references and captions contain no markup and no control characters. A link's
document ID is lower than the pack's document count.

Example (`\x01` etc. are the bytes):

```
\x011Hypothermia
Hypothermia is a body temperature below 35 °C. See \x0217\x1Ffrostbite\x03.
\x04hypothermia-stages\x1FThe stages of hypothermia
\x012Treatment
Move the person somewhere warm.
```

**Plain text** (`arcpack_plain()`), used for indexing, embedding and the model's context: the heading marker and
level are removed (the heading text stays), a link becomes its text, an image line becomes its caption. Every
other byte is kept, so plain text is never longer than the marked-up text.

## Manifest

`manifest.json` follows `schemas/pack.schema.json`. A reader also checks that its `name` and `version` equal every
file header's `pack_name` and `pack_version`, that `chunk_count` equals the count in `chunks/chunks.idx`, and that
`document_count`, when present, equals the count in `content/docs.idx`.

## Costs

32 bytes per document and per chunk, plus the strings and the compressed text. Document and chunk lookup is O(1)
by index, and a chunk ID resolves in O(1) through `first_chunk`. Opening a pack checks headers only, never every
record. Reading a chunk decompresses its one document into the caller's buffer; nothing scales with the pack's size
except the mapped files, which live in the page cache.
