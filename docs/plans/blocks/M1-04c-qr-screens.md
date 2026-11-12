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
