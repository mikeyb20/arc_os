# ADR-0013: UTF-8 everywhere

- **Status:** Accepted
- **Date:** 2026-11-07

## Decision
- All text in the kernel console, TTY, libc, file names and packs is UTF-8. libc offers only the `C.UTF-8` locale.
- Console fonts use the PSF2 format with a Unicode mapping table; the default font covers Latin-1, Latin Extended-A,
  common punctuation and symbols (°, µ, ±, ×, ÷, arrows, box drawing). Missing glyphs render as a replacement box.
- The ANSI/VT parser decodes UTF-8 before rendering.

## Consequences
- Medical units and accented names display correctly. Rules out: other encodings.

## Verification
- Host tests for the UTF-8 decoder (including invalid sequences); QEMU screenshot test of a symbol line.
