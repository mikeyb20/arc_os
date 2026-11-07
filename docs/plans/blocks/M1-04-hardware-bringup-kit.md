# M1-04 — Real-hardware bring-up kit and first hardware boot

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K + H | M1-01, M1-02, M1-03 | M1-22 |

**Goal.** arc_os boots on the laptop and the mini PC from a USB stick, and what it finds is readable from the
screen, so hardware surprises show up before the rest of M1 is built on top.

**Read first.**
- `kernel/drivers/fb_console.c`, `fb_console.h`, `kernel/lib/kprintf.c`, `kernel/arch/x86_64/isr.c` (panic path),
  `kernel/drivers/acpi.c`, `kernel/drivers/pci.c`, `tools/make-iso.sh`, `limine.conf`
- Notes of M1-01, M1-02, M1-03

**Build.**
1. Kernel log ring buffer (256 KiB) behind kprintf; framebuffer scrollback (PgUp/PgDn when a keyboard works);
   `/proc/kmsg`-style read.
2. Panic screen: reason, registers, CR2, CPU index, symbolised backtrace (symbol table generated at build time and
   linked in or loaded as a Limine module), in a colour block that photographs well.
3. Boot report on one screen, paged automatically every 10 s when no key is pressed: CPU model, features, core
   types and count, memory map summary, ACPI tables, APIC mode, timer calibration, PCI devices
   (`vendor:device`, class, bound driver or "no driver"), framebuffer mode.
4. Self-test (`arc.selftest=1`): wake all CPUs, AVX2 checksum on each, 256 MB memory pattern test, timer check
   against the TSC; PASS/FAIL per line.
5. USB image: the hybrid ISO from `make-iso.sh` with boot entries for normal boot and self-test.
   `docs/hardware/bringup.md`: how to write the stick (`dd` or Etcher), firmware settings (UEFI on, Secure Boot
   off, legacy USB support on where offered), what to photograph.
6. QEMU tests: a forced panic (test-only command line flag) shows the panic markers on serial; the boot report
   markers are present.

**Done when.**
- [ ] QEMU tests pass.
- [ ] **Michael:** both machines boot the stick; photos of the boot report and self-test are posted in the PR.
      Small fixes go in this block; anything bigger becomes a new block in work-blocks.md.

**Watch out.**
- Mini PC: no USB keyboard until xHCI (M4-01) unless the firmware's legacy USB emulation is on. Nothing in the
  report may need input.
- Laptop: no serial port; the screen is the only channel.
- Framebuffers differ (pitch ≠ width × 4, BGR vs RGB). Use the Limine framebuffer masks.

**Notes.**
-
