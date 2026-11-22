# M3-03a — Own ZIM reader (host)

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | C | M2-02, M3-01 | M3-03 |

**Goal.** Read Kiwix ZIM files on the build host with our own code instead of python-libzim (Michael, 2026-11-22,
ADR-0022): a C library `arczim` and a `zimcat` tool in `stack/`, on the archived zstd port, with a test ZIM writer
so no test downloads a ZIM. Prove it on a small real medical ZIM. Spec written with the block, after Michael's
choice; it splits the reader out of M3-03, which was already L.

**Read first.**
- `docs/plans/content-lane.md`, `docs/plans/lane-l.md`, `stack/README.md`
- openZIM's "ZIM file format" page (archived as `zim-spec`)

**Build.**
1. **`stack/lib/arczim/`**: map the file, check the header, MIME list and pointer-list extents at open; read
   dirents by number and by path (binary search; lower bound for namespace ranges); follow redirects (32 hops, loop
   is an error); read blobs from uncompressed clusters in place and from zstd clusters through an LRU cache
   (window ≤ 2^27, cluster ≤ 512 MiB); refuse xz, zlib, bzip2 and unknown codes naming the cluster; metadata by key.
   `arczim_check` reads the whole file once (dirents, path order, redirects, every cluster's blob table, every blob
   number) and counts entries per namespace and MIME type, clusters per compression, and how many cluster
   decompressions path-order reading of the HTML would cost.
2. **`zimcat info | ls [NS] | cat NS/PATH | check`** (`stack/zimcat/`). Control characters printed as `\xNN`.
3. **Test writer** `stack/tests/zim/zim_fixture.{c,h}` (library) and `zim_fixture` (JSON spec → ZIM; can generate
   many articles spread over clusters out of path order). M3-03's extractor test uses it too.
4. **Lane tests**: `lane.arczim` (good v6 file, v5 file, refused compressions, 11 damaged headers, 17 damaged
   dirents, pointer lists, clusters and blob tables, 1500 randomly damaged copies) and `lane.zimcat`; budgets
   `zimcat_bytes`, `zimcat_check_rss_kib`.
5. **Docs**: ADR-0022, `docs/formats/zim.md`, the `zim-spec` entry in `SOURCES.lock` and `LICENSES.md`, the
   reader in content-lane.md and the M3-03 spec, `stack/README.md` and `lane-l.md` rows.
6. **Sample**: download one small English medical ZIM from the catalog, check its SHA-256 against Kiwix's
   `.sha256`, keep it in `/mnt/project-files/arc_os/content/sample/` (content is never in git), run `zimcat check`.

**Done when.**
- [x] Lane tests pass under the syscall check; budgets recorded.
- [x] `zimcat check` passes on the sample ZIMs and they use no xz clusters.
- [x] ADR-0022, `docs/formats/zim.md` and the plan updates are in.

**Cost.** Opening: O(1) in the file size plus the MIME list. Entry by number O(1), by path O(log entries) dirent
reads. A blob from a cached cluster O(1); a miss decompresses one cluster (about 2 MiB of text in the samples).
Memory: the cache slots (default 4, each as big as the largest cluster read). `arczim_check`: 8 bytes per content
entry + 16 per cluster + one cluster buffer (it uses one slot).

**Watch out.**
- Nothing that runs on arc_os may link `arczim` (ADR-0011).
- libzim's zstd frames have no checksum and arczim doesn't check the ZIM's MD5: the SHA-256 check before a build is
  what catches a damaged file (content-lane.md).
- Path order is not cluster order (Notes): don't extract in path order.

**Notes.**
- **Samples** (2026-11-22, `/mnt/project-files/arc_os/content/sample/`, SHA-256 match Kiwix's `.sha256` files).
  `download.kiwix.org` answered from the cloud session (it was blocked on 2026-11-11).

  | | WikiMed mini 2026-10 | NHS Medicines 2026-09b |
  |---|---|---|
  | File | `wikipedia_en_medicine_mini_2026-10.zim`, 174 350 854 bytes, `c66f2727…ddac0822` | `nhs.uk_en_medicines_2026-09b.zim`, 15 322 705 bytes, `349f7bc8…b6668b62` |
  | Made by | mwoffliner 2.1.1 (Wikipedia scraper) | zimit 3.1.3 / warc2zim 2.3.1 (web crawl) |
  | ZIM version | 6.3, no title list, main page `W/mainPage` | 6.3, same |
  | Entries | 340 829: C 340 812, M 13, W 1, X 3 | 1 436: C 1 420, M 12, W 1, X 3 |
  | Content / redirects | 73 429 / 267 400 | 1 425 / 11 |
  | HTML articles | 72 901 (one `text/html; charset=iso-8859-1`), 1 053 427 894 bytes, ~14.4 KB each | 1 347, 52 514 656 bytes, ~39 KB each (whole site pages) |
  | Other content | 442 SVG, 30 CSS, 32 JS, 5 PNG, 2 Xapian indexes (88.6 MB), 1 title listing | 9 JPEG, 2 PNG, 21 MP2T video segments (10.3 MB), scripts, styles |
  | Clusters | 511: 507 zstd, 4 uncompressed, none extended | 35: 27 zstd, 8 uncompressed |
  | Decompressed | 1 146 791 373 bytes; largest cluster 57 229 320 (uncompressed Xapian) | 67 037 281; largest 2 095 418 |
  | Blobs | 73 429, none unreferenced or shared | 1 424, 1 shared (an alias, minor 2) |
  | Path-order decompressions (cache 1 / 4 / 16 / 64) | 72 741 / 72 287 / 70 541 / 63 716 | 390 / 53 / 36 / 27 |
  | `zimcat check` | ok, 0.53 s, peak RSS 87 MB (mostly mapped file pages) | ok, 0.03 s, 7 MB |

- **No xz anywhere**: both use only zstd and uncompressed clusters, as expected for libzim ≥ 7.
- **Reading order is the big finding.** WikiMed's articles sit in clusters in scrape order, so reading them in path
  order decompresses a cluster about once per article even with 64 cache slots: 63 716 decompressions of ~2 MiB,
  roughly 130 GB of zstd work for a 174 MB file, against 507 in cluster order. M3-03's spec now has `zimcat stream`
  emit in cluster order and the extractor sort afterwards. The zimit ZIM has much better locality (whole-site crawl
  order), so this depends on the scraper.
- **Licence metadata is missing.** Neither ZIM has a `License` key (WikiMed: Name, Title, Creator, Publisher, Date,
  Description, Flavour, Source, Scraper, Language, Tags, Counter, Illustration). M3-01 hoped M3-03 could fill the
  blank licences from it; it can't for these. Wikipedia text is CC BY-SA 4.0 (Wikipedia's licence page, industry
  knowledge, not read from the ZIM); NHS is OGL-UK-3.0 per M3-01. The [VERIFY] flag in the content list stays.
- The catalogue's `article_count` for WikiMed mini (340 299) is close to the entry count, not the 72 901 HTML
  articles: use `zimcat check` for real numbers.
- WikiMed mini has no images (`_pictures:no`); the image-type question for M3-08 needs the maxi file (M3-03).
- The test pack Michael asked for comes from M3-03 (extractor + `packbuild`), built first on the WikiMed mini sample.
