# ADR-0022: Our own ZIM reader on the build host

- **Status:** Accepted (Michael, 2026-11-22: "1b" on the coordinator's question in the project chat, "write our own
  reader" instead of python-libzim)
- **Date:** 2026-11-22
- **Amends:** [`content-lane.md`](../plans/content-lane.md) ("Where the line between host-only and C is") and the
  M3-03 spec, which left the choice open (python-libzim unless Michael chose an in-house reader). Adds block M3-03a.
  Supersedes no ADR; ADR-0011 is unchanged.

## Context
- The content lane turns Kiwix ZIM files into packs on a Linux build host (ADR-0011: arc_os never parses a source
  format). M3-00a planned to read ZIMs with python-libzim, Kiwix's library, unless Michael chose an in-house reader.
- python-libzim would be one more outside component to archive and keep buildable offline (libzim is C++ with
  ICU, Xapian and zstd/xz dependencies, plus the Python binding). The project's rule is that no build may need a
  third-party server.
- The ZIM container is small and well specified: an 80-byte header, three pointer lists, directory entries and
  clusters (`docs/formats/zim.md`). Every ZIM on the approved content list (M3-01, dates 2024-11 to 2026-11) was
  written by libzim 7 or later, which compresses clusters with zstd. We already archive and build zstd 1.5.7
  (M2-02).
- Python's standard library has no zstd before 3.14, so a pure-Python reader would still need a zstd binding.

## Decision
1. **Read ZIMs with our own C library, `arczim`, in `stack/lib/arczim/`, and the `zimcat` tool** (M3-03a). Static
   musl like the rest of `stack/`, linked with the archived zstd port, tested in lane L.
2. **Build host only.** Nothing that runs on arc_os links `arczim` (ADR-0011 stands). It lives in `stack/` because
   that is where the pipeline's C code, the zstd port and the lane tests are.
3. **zstd and uncompressed clusters only.** xz (LZMA2) clusters are refused with an error naming the cluster; zlib
   and bzip2 were removed from the format long ago. Supporting xz would mean archiving xz-utils, for files we don't
   use. If a ZIM we want ever has xz clusters, that is a new decision: archive xz-utils, or recompress the ZIM.
4. **The reader trusts nothing.** Every offset, count and string is checked against the mapped file before use;
   a damaged file gives an error, never a crash or an out-of-bounds read. `arczim_check` (`zimcat check`) reads
   the whole file once and checks every dirent, the path order, every redirect chain and every cluster's blob
   table.
5. **No MD5.** The ZIM's own MD5 isn't verified; the file's SHA-256, checked against Kiwix's published `.sha256`
   and recorded in `sources.json`, is the integrity check. libzim's zstd frames carry no checksum, so this matters.
6. **HTML stays in Python.** The extractor that turns article HTML into pack markup (M3-03) remains host-only
   Python with the standard library's `html.parser`; it gets entries from `zimcat`, not from python-libzim.
   python-libzim is not archived and not used anywhere.
7. **Test ZIMs come from our own writer** (`stack/tests/zim/zim_fixture`), so no test downloads a ZIM and none
   needs libzim to make one. M3-03's extractor test uses it too.
8. The format as we read it is `docs/formats/zim.md`; openZIM's spec pages are archived as `zim-spec`.

## Consequences
- One less outside component; the reader is about 600 lines of C we can read and fix.
- New ZIM features need our code to follow. The minor versions so far (6.1 to 6.4) changed no byte layout we
  read; a major version 7 would be refused at open.
- `zimcat check` reports how many cluster decompressions reading articles in path order would cost. On the WikiMed
  sample it is almost one per article, so M3-03 must read in cluster order (M3-03a Notes).
- The extractor and `zimcat` run as two processes connected by a pipe (M3-03 adds the stream command).

## Verification
- Lane tests `lane.arczim` (good, v5, damaged and 1500 randomly damaged fixture files) and `lane.zimcat` (the
  commands on fixtures, xz refused), under the syscall check; budgets `zimcat_bytes`, `zimcat_check_rss_kib`.
- `zimcat check` passes on WikiMed mini 2026-10 and NHS Medicines 2026-09b (M3-03a Notes); both use only zstd and
  uncompressed clusters.
