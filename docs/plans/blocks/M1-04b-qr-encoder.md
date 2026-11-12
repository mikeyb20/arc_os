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
- [ ] `ctest --test-dir build-tests -L host -R qr` passes, including the round trip for every version 1–30.
- [ ] The kernel builds with the new files; `tools/no-simd-check.py` and `tools/hygiene.py` pass with no count
      raised (every function under 100 lines).
- [ ] Notes say what M1-04c needs to know (API, buffer sizes, measured encode time on the host).

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
