# M1-04b — QR encoder (host-tested)

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K | M1-04 | M1-04c |

**Goal.** A small, allocation-free QR encoder for plain text, proven byte for byte against a reference decoder on
the build host. M1-04c then uses it to draw the boot report, the log and the panic screen as codes, so hardware
results can be read off the screen exactly (Michael, 2026-11-12; [`parallel-lanes.md`](../parallel-lanes.md) §6.4).

This block adds new files only. It touches no scheduler, lock, console or `kprintf` code, so it runs beside M1-05.
Nothing in the kernel calls the encoder yet. The only shared files are add-only lists (`kernel/CMakeLists.txt`,
`tests/CMakeLists.txt`, `docs/kernel/helpers.md`, `tools/dev-setup.sh`).

**Read first.**
- [`docs/plans/parallel-lanes.md`](../parallel-lanes.md) §6.4 (payload format and sizes)
- `docs/kernel/helpers.md`, `kernel/diag/diag.h`, `kernel/CMakeLists.txt`, `tests/CMakeLists.txt`,
  `tests/test_kprintf.c` (how a host suite is laid out), `tools/dev-setup.sh` (the apt list)
- ISO/IEC 18004 (QR Code) tables for versions 1–30 at error-correction level M: you know them, but every table
  value must be proven by the round-trip test, not trusted.

**Build.**
1. `kernel/lib/crc32.c`, `crc32.h`: CRC-32 (IEEE 802.3, reflected polynomial `0xEDB88320`, init and final XOR
   `0xFFFFFFFF`) with a 1 KiB `const` table. Listed in `docs/kernel/helpers.md`. The check value of `"123456789"`
   is `0xCBF43926`.
2. `kernel/diag/qr.c`, `qr.h`: `qr_encode(const uint8_t *data, size_t len, int max_version, QrCode *out)`.
   - Byte mode, error-correction level M, versions 1–30. It picks the smallest version that fits, up to
     `max_version`, and returns that version, or `-E2BIG`.
   - Reed–Solomon over GF(256) (polynomial `0x11D`, `const` log and antilog tables).
   - Block interleaving, function patterns (finders, timing, alignment, dark module), format bits (BCH) and
     version bits (versions 7+).
   - All 8 masks, scored with the standard's four penalty rules, keeping the lowest.
   - `QrCode` is caller-owned: a module bitmap of at most 137 × 137 bits plus its work area. No allocation, no
     globals, no locks, no floating point, so the panic path can call it.
3. `kernel/diag/qr_text.c`, `qr_text.h`: pure helpers that M1-04c will draw.
   - `qr_text_sanitize`: keeps printable ASCII and `\n`. Any other byte becomes `\xNN`, and `://` becomes
     `:\/\/`, so a code can never carry a link.
   - `qr_text_paginate`: splits text at line boundaries into pages that each fit one code at `max_version`. Each
     page is prefixed with `ARC1 <commit> <kind> <page>/<pages> <crc32-hex of this page's text>\n`. The last
     page adds one line, `all <crc32-hex of all pages' text>`.
4. Host tests `tests/test_qr.c` (suite `qr`):
   - Known-answer tests: CRC-32, GF(256) tables, the 32 format-information words and the version-information words
     of versions 7–30 against the standard's tables, and the data-codeword capacity per version (version 30-M must
     be 1,370 bytes).
   - Round trip, about 50 inputs:
     - empty input and 1 byte;
     - exactly the capacity of every version 1–30;
     - capacity + 1, which must return `-E2BIG`;
     - all 256 byte values;
     - random text with a fixed seed;
     - a real QEMU `[REPORT]` block.
   - Each input is rendered to a PGM (4 px per module, 4-module quiet zone) and decoded with `zbarimg --raw -q
     -Sbinary`. The decoded output must match byte for byte.
   - Sanitizer and paginator cases: control bytes, `://`, a line longer than one page (split, never dropped), and
     a CRC per page plus over all pages.
5. `tools/dev-setup.sh` installs `zbar-tools` (Ubuntu, host-only test tool like QEMU). The round-trip test fails
   with "run tools/dev-setup.sh apt" if `zbarimg` is missing; it never skips.

**Done when.**
- [x] `ctest --test-dir build-tests -L host -R qr` passes, including the round trip for every version 1–30.
- [x] The kernel builds with the new files; `tools/no-simd-check.py` and `tools/hygiene.py` pass with no count
      raised (every function under 100 lines).
- [x] Notes say what M1-04c needs to know (API, buffer sizes, measured encode time on the host).

**Cost.** One encode is linear in the module count for each of the 8 masks: about 8 × 19k modules × 4 rules at
version 30, around a million simple operations. It runs only on diagnostic and panic paths, never on a hot path.
Memory: the caller's `QrCode` (about 6 KiB at version 30), static in M1-04c.

**Watch out.**
- Kernel flags (`-mgeneral-regs-only`, freestanding): no `float`, no libc. The host test compiles the same `.c`
  files with system GCC.
- Version 7+ needs version-information blocks, and alignment-pattern positions differ per version. These are the
  usual sources of codes that scan on one phone and not another. The decoder round trip over every version is the
  guard.
- The mask penalty rules are easy to get subtly wrong without breaking decoding. A wrong score only picks a worse
  mask, but test rule 3 (finder-like patterns) against a known example anyway.
- No `zbar` code goes into the kernel. It's only the reference decoder on the build host.

**Notes.**
- Built: `kernel/lib/crc32.{c,h}` (`crc32(crc, data, len)`, zlib-style chaining), `kernel/diag/qr.{c,h}` and
  `kernel/diag/qr_text.{c,h}`, host suite `qr` (22 tests, about 60 `zbarimg` decodes, 1.4 s). Nothing in the
  kernel calls them yet.
- For M1-04c: keep one `static QrCode` (7,132 bytes, not the 6 KiB estimated above; never on a kernel stack).
  `qr_encode(buf, len, 30, &qr)` returns the version; draw `qr_module(&qr, x, y)` for x, y in `0..qr.size-1` plus
  a 4-module light border. Text: `qr_text_sanitize` into a buffer (an escape is 4 bytes, so worst case 4 × the
  input), then `qr_text_paginate` into a `QrTextPage` array, then `qr_text_page` into a `char[QR_BYTES_MAX]` per
  page. The QEMU report (1,981 bytes) is 2 pages at version 30. Every payload fits `qr_capacity(max_version)`.
- Encode time on the build host (Xeon 2.1 GHz): version 30 = 4.1 ms at -O2 and 8.9 ms at -O0 (the kernel builds
  without -O); version 10 = 0.6 / 1.6 ms. The mask penalty dominates. The Cost estimate above (about a million
  operations) was low by a few times; it still only runs on diagnostic and panic paths.
- Kernel image grows about 11 KB (qr 8.3 KB, qr_text 2.5 KB, crc32 1.2 KB of text, plus backtrace names):
  328,564 on top of M1-05's 317,173, so `kernel_image_bytes` went from 310,000 to 345,000 (measured + 5%) with a
  reason line in `tests/budgets.txt`. M1-04c adds drawing code and the 7 KB static `QrCode` (bss counts).
- Choices: mask rule 3 counts the literal 11-module windows `10111010000` and `00001011101`, with modules outside
  the symbol counted as light (they are the quiet zone); scaled look-alikes are not counted. A different reading
  only picks a different mask, and every mask decodes (tested). Pages reserve room for the longest header their
  page count allows and for the `all` line on every page, so the split never changes (about 1% unused). If the
  text doesn't end in a newline, one goes before the `all` line and belongs to no page's CRC.
- Proof beyond the round trip: a decoder repairs a few wrong modules, so tests also read both format copies and
  every data bit back by the standard's layout and demand an exact match, and check every block's Reed–Solomon
  syndromes are zero. Mutation runs (not in CI) confirmed the suite catches a wrong mask formula, one flipped
  format or data bit, a wrong version-bit layout, a skipped column, one-sided rule 3, wrong rules 1, 2 and 4, and
  picking the worst mask. The terminator is invisible in byte mode (data always ends 4 bits short of a byte, and
  the byte padding supplies those zeros), so no test can see it; it stays for the standard.
- `zbar-tools` is a host-only test tool from Ubuntu's apt, like QEMU and xorriso. Neither the kernel build nor any
  shipped file needs it. It joins the known gap that apt host packages aren't archived for an offline rebuild
  (owner M5-04).
