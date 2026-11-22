# ZIM, as arc_os reads it

Written by M3-03a under [ADR-0022](../decisions/0022-own-zim-reader.md). This is our summary of the part of
openZIM's format that `stack/lib/arczim/` reads, in our words. The authority is openZIM's own spec, archived as
`zim-spec` in `third_party/SOURCES.lock` (five wiki pages at fixed revisions: "ZIM file format" r31500,
"Metadata", "Search indexes", "Well known entries", "Article Format"). If the two disagree, the archived spec wins
and this file is wrong.

ZIM is Kiwix's archive format: a header, some index lists, directory entries ("dirents") and compressed
clusters that hold the entries' data. arc_os only reads ZIMs on the Linux build host, to make packs
(ADR-0011): no ZIM ever reaches the computer.

## Conventions

- Little-endian, unsigned integers. Offsets are from the start of the file and 64-bit.
- Strings are UTF-8 and NUL-terminated. arczim also caps each one at 64 KiB, so a damaged file can't make it scan
  far.
- Everything a reader needs lies before `checksumPos`; the last 16 bytes are an MD5 of everything before them.
  arczim treats `checksumPos` as the end of the data and doesn't check the MD5 (ADR-0022: the download's SHA-256
  against Kiwix's `.sha256` file is the integrity check).

## Header (80 bytes)

| Offset | Size | Field | arczim checks |
|---|---|---|---|
| 0 | 4 | `magicNumber` = 72173914 (`0x044D495A`) | equal |
| 4 | 2 | `majorVersion` | 5 or 6 |
| 6 | 2 | `minorVersion` | read only (6.1 new namespaces, 6.2 aliases, 6.3 no title list, 6.4 no dot segments) |
| 8 | 16 | `uuid` | — |
| 24 | 4 | `entryCount` | the path pointer list fits |
| 28 | 4 | `clusterCount` | the cluster pointer list fits |
| 32 | 8 | `pathPtrPos` | `entryCount × 8` bytes before `checksumPos` |
| 40 | 8 | `titlePtrPos` | not used; all ones when absent (minor ≥ 3) |
| 48 | 8 | `clusterPtrPos` | `clusterCount × 8` bytes before `checksumPos` |
| 56 | 8 | `mimeListPos` | ≥ 80 and before `checksumPos` (it is also the header's size) |
| 64 | 4 | `mainPage` | `0xFFFFFFFF` or an entry number |
| 68 | 4 | `layoutPage` | not used (deprecated) |
| 72 | 8 | `checksumPos` | exactly 16 bytes or more before the end of the file |

## MIME list

NUL-terminated strings one after another from `mimeListPos`, ended by an empty string. A content dirent names
its type by index. Types can carry parameters (`text/html; charset=iso-8859-1` appears in WikiMed); arczim treats
any type starting `text/html` followed by the end or `;` as HTML.

## Path pointer list and entry numbers

`entryCount` 64-bit offsets of dirents, sorted by namespace byte then path bytes (`memcmp` order). An entry's
**number** is its index in this list, so entry numbers are path order. arczim finds a path by binary search
(`arczim_find`, `arczim_lower_bound`); `arczim_check` verifies that the order is strictly increasing, because a
file out of order would make the search miss entries silently.

The title pointer list (`titlePtrPos`, 32-bit entry numbers by title) is obsolete and absent from 6.3 files; arczim
doesn't read it.

## Directory entries

| Offset | Size | Content entry | Redirect (`mimetype` = `0xFFFF`) | Obsolete (`0xFFFE`, `0xFFFD`) |
|---|---|---|---|---|
| 0 | 2 | `mimetype` (MIME list index) | `0xFFFF` | link target / deleted |
| 2 | 1 | `parameterLen` (bytes after the title, always 0 in practice) | same | same |
| 3 | 1 | `namespace` | same | same |
| 4 | 4 | `revision` (unused) | same | same |
| 8 | 4 | `clusterNumber` | `redirectIndex` (entry number) | path starts here |
| 12 | 4 | `blobNumber` | path starts here | |
| 16 | | path, title | | |

An empty title means "same as the path". arczim checks the MIME index, the cluster number and the redirect target
against their counts, finds both NULs before `checksumPos`, and checks that the parameter bytes fit. Redirects are
followed for at most 32 hops; more is reported as a loop. Obsolete entries (never seen in a current ZIM) carry
no data and are skipped.

**Namespaces** since minor 1: `C` content (articles, images, CSS, scripts), `M` metadata, `W` well-known entries
(`W/mainPage`), `X` search indexes (Xapian databases, `X/listing/titleOrdered/v1`). Older files use `A` articles,
`I` images, `-` layout and others; arczim reads either, without caring which.

## Clusters

`clusterPtrPos` holds `clusterCount` 64-bit offsets. A cluster starts with one **info byte**: the low 4 bits are
the compression, bit 4 (`0x10`) marks an **extended** cluster (blob offsets are 8 bytes instead of 4; major
version 6 only). Higher bits must be zero.

| Code | Compression | arczim |
|---|---|---|
| 0, 1 | none | read in place from the mapping |
| 2, 3 | zlib, bzip2 (removed from the format) | refused |
| 4 | xz / LZMA2 | **refused** (ADR-0022); no ZIM on the content list uses it |
| 5 | zstd | one frame, decompressed into an LRU cache of cluster buffers |

After the info byte (inside the compression) comes a **blob offset table** and the blobs: `n + 1` offsets, each
from the start of the table; the first offset is the table's own size (so `n = first / offset size − 1`), and blob
`i` is `[offset[i], offset[i+1])`. arczim checks that the first offset is a non-zero multiple of the offset size,
and that a blob's offsets are in order and inside the decompressed data; `arczim_check` checks the whole table of
every cluster.

The end of a cluster is not stored. zstd frames end by themselves, and an uncompressed cluster's table gives its
size, so arczim only bounds the input by `checksumPos`.

**zstd limits.** Decompression is streaming with `ZSTD_d_windowLogMax` = 27 (128 MiB; libzim writes level-19
frames with an 8 MiB window), and a cluster may decompress to at most 512 MiB (`arczim_set_cluster_max`). libzim's
frames carry **no content checksum**, so a flipped bit inside compressed data can decode to wrong bytes without an
error. That is why the build checks the file's SHA-256 before using it.

## Metadata

Entries `M/<key>` with `text/plain` values (`M/Illustration_48x48@1` is a PNG). Keys seen in the samples:
`Name`, `Title`, `Creator`, `Publisher`, `Date`, `Description`, `Language`, `Tags`, `Flavour`, `Source`, `Scraper`,
`Counter` (entries per MIME type). `License` is optional and **absent from both samples** (M3-03a Notes).

## Reading order matters

Entries are numbered in path order, but their data sits in clusters in the order the scraper wrote them. In the
WikiMed sample, reading the 72 901 articles in path order would decompress a cluster 72 741 times with a one-slot
cache and 63 716 times with 64 slots, for 507 clusters (M3-03a Notes). Read in cluster order instead: every
cluster once. `zimcat check` prints these numbers (`html path-order decompressions`) for any ZIM.
