# M1-04c — Report, log and panic as QR codes

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| S | R | KB | M1-04b, M1-05 | exact text from every later hardware boot |

**Goal.** The bring-up stick can show the boot report, the self-test, the benchmark and the newest kernel log as
plain-text QR codes. A panic screen carries one code. Michael's phone reads the codes, and he pastes the text into
the thread, so hardware results arrive exactly, with no transcription
([`parallel-lanes.md`](../parallel-lanes.md) §6.4).

It needs M1-05 because M1-05 makes `kprintf` and the log ring SMP-safe and settles who owns the console, and this
block reads the ring and pauses the console.

**Read first.**
- Notes of M1-04, M1-04b and M1-05; `kernel/diag/report.c`, `diag.h`, `kernel/drivers/fb_console.c`, `fb_console.h`,
  `kernel/lib/klog.c`, the panic screen code, `tools/make-iso.sh`, `docs/hardware/bringup.md`, `tools/qemu-test.py`

**Build.**
1. `fb_console`: pause and resume, and `fb_console_draw_bitmap()` to draw a 1-bit bitmap scaled to whole pixels
   per module and centered, black on white, with a 4-module quiet zone. It never reads the framebuffer.
2. `arc.qr=1` draws the report, self-test and bench text as QR pages; `arc.qr=log` draws the newest 8 KiB of the
   log ring.
   - Both use `qr_text_paginate` with version 30 at most, and kind `report` or `log`.
   - Under each code, a text line shows `page n/N` and the CRC.
   - Pages turn every 10 s or on any key, and repeat.
   - Init never starts, as with `arc.report`.
3. Panic screen: one code (kind `panic`) holding the reason, registers, CR2, CPU and backtrace, drawn beside the
   text. It uses a static `QrCode` and never allocates. If the text doesn't fit, the backtrace is cut from the
   bottom and the code says so.
4. The report's first line gains the git commit, `git describe --always --dirty` passed by CMake (`unknown`
   outside git). The Features line gains `TSC-DEADLINE` (CPUID 1 ECX bit 24), `ARAT` (CPUID 6 EAX bit 2), `HWP`
   (CPUID 6 EAX bit 7) and `EPP` (CPUID 6 EAX bit 10). RAPL has no CPUID bit on Intel, so M1-21 reports it.
5. `make-iso.sh -B`: a fourth menu entry, "boot report as QR codes" (`arc.qr=1`), and a fifth, "kernel log as QR
   codes" (`arc.qr=log`).
6. QEMU suite `qr`:
   - Boot with `arc.qr=1`, take a QEMU `screendump` of the first page (add a monitor socket to `qemu-test.py`
     if it has none) and decode it with `zbarimg`.
   - The text must equal that page's `[REPORT]` lines on serial and match its CRC.
   - The `kpanic` suite also decodes the panic code.
7. `docs/hardware/bringup.md`: how to scan (camera preview, copy the text, paste it in the thread). The safety
   habit: never tap a code that offers to open a website, join a network or call. arc_os's codes always start with
   `ARC1` and never hold a link.

**Done when.**
- [ ] QEMU suite `qr` passes; the panic code decodes.
- [ ] Host tests and the full QEMU suite pass; hygiene counts unchanged.
- [ ] The next laptop smoke boot (work-blocks §3 rule 10) is read by QR, and its text goes in
      `docs/hardware/<machine>.md`.

**Watch out.**
- Screens: version 30 is 145 modules with the quiet zone. That's 5 px per module at 768 px high and 9 px at 1350
  px. Pick the scale from the shorter screen side and never go under 4 px.
- Some framebuffers are BGR, and the pitch isn't width × 4: draw through the existing mask-packed path.
- The panic path must not take the console lock if a CPU died holding it (M1-05's panic stop rules).

**Notes.**
- Built out of lane order, beside M1-09 (Michael's card, 2026-11-13; work-blocks §5). Shared file with M1-09:
  `report.c`, split by function (M1-09 rewrites `report_pci`; this block the first line, the Features line, the
  pager and the exported helpers). `qemu-test.py` gains `--screendump`/`--after-shot`/`--check`; M1-09 adds
  `--qemu-arg`: whichever merges second rebases.
- New: `diag/qr_screen.{c,h}` (layout, page and panic drawing, panic and log texts; host suite `qr_screen`),
  `diag/qr_mode.c` (the `arc.qr` loop and the panic hook), `fb_console` pause/resume, fill, paint_row and
  `draw_bitmap`, `klog_for_each_line` (the log-line walker moved out of `fb_console.c`, which now uses it),
  `lib/build_commit.h` (`cmake/build-commit.cmake`, rewritten only when the commit changes), `tools/qr-check.py`.
- Choices: the commit is `git describe --always --dirty --abbrev=12 --exclude=*` (no tag names, so headers always
  name a commit). The QR menu entry runs `arc.qr=1 arc.selftest=1 arc.bench=1`, so it replaces the default
  entry's photos. The pager pauses the console instead of removing the log hook, and draws with the console
  owner held, waiting with it released. The largest QR version is picked from the screen (version 30 needs
  580 px at 4 px per module); below 4 px the text pager runs instead. The panic code uses 4 px modules (8 on 2x
  screens), top right, and the panic view redraws the log wrapped (at a space) to the columns left of it, so no
  register or frame is hidden under the code on any screen at least 40 columns wide beside it (the D3 review
  found CR2 hidden at 1024x768 with a 16-frame trace before this); `panic_set_screen` now passes the
  `PanicInfo`, and test mode prints `ARC_TEST PANIC` before the screen. `arc.qrhold=1` (test mode only) holds
  the panic screen 60 s for the screendump; the harness ends QEMU.
- Tests: QEMU `qemu.qr.report` (2 pages), `qemu.qr.log` (7 pages), `qemu.qr.panic` decode every screendump with
  `zbarimg` and match serial byte for byte, CRCs included (TCG about 3-8 s each). Mutations checked by hand:
  an edited serial line, swapped or missing pages and a changed register all fail `qr-check.py`.
- Size: kernel image 328,564 -> 348,042 bytes (11.6 KB .bss for the static panic buffers, 7.9 KB code); limit
  345,000 -> 365,000 in `tests/budgets.txt`.
- Not done here: the laptop smoke boot read by QR (Done when, item 3) is Michael's; console locking unchanged
  (the TTY still draws outside the console owner until M1-09 takes TTY/VT off the kernel lock; that block
  should also list the QR pager, which draws even when `klog_console_acquire` times out, as VT and TTY do).
  Report and log codes use every text row but the caption's (`qr_screen_page_height`), so on a height that
  isn't a whole number of rows the caption no longer covers the quiet zone. `klog_for_each_line` keeps the
  line `from` starts on (it used to skip it). Serial carries lines the log doesn't (early
  `[BOOT] stage:` lines go straight to the port), so the log check matches lines in order, not one run.
