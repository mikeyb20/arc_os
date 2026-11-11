# M1-04 — Real-hardware bring-up kit and first hardware boot

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K + H | M1-01, M1-02, M1-03 | M1-22 |

**Goal.** arc_os boots on the laptop and the low-tier machine from a USB stick, and what it finds is readable from the
screen, so hardware surprises show up before the rest of M1 is built on top.

**Read first.**
- `kernel/drivers/fb_console.c`, `fb_console.h`, `kernel/lib/kprintf.c`, `kernel/arch/x86_64/isr.c` (panic path),
  `kernel/drivers/acpi.c`, `kernel/drivers/pci.c`, `tools/make-iso.sh`, `limine.conf`
- `docs/decisions/0018-portable-usb-mode.md` (this image is its RAM mode), Notes of M1-01, M1-02, M1-03

**Build.**
1. Kernel log ring buffer (256 KiB) behind kprintf, with log levels; the serial and screen consoles drain it
   without blocking the caller, and hot paths log at debug level only (ADR-0019); framebuffer scrollback (PgUp/PgDn when a keyboard works);
   `/proc/kmsg`-style read.
2. Panic screen: reason, registers, CR2, CPU index, symbolised backtrace (symbol table generated at build time and
   linked in or loaded as a Limine module), in a colour block that photographs well.
3. Boot report on one screen, paged automatically every 10 s when no key is pressed: machine maker and model
   (SMBIOS type 1, through Limine's SMBIOS request), CPU model, features, core types and count, memory map summary,
   ACPI tables, APIC mode, timer calibration, PCI devices (`vendor:device`, class, bound driver or "no driver"),
   framebuffer mode.
4. Self-test (`arc.selftest=1`): wake all CPUs, AVX2 checksum on each, 256 MB memory pattern test, timer check
   against the TSC; PASS/FAIL per line.
5. USB image: the hybrid ISO from `make-iso.sh` with boot entries for normal boot and self-test. Everything runs
   from RAM (ADR-0018's RAM mode), so no USB driver is needed yet and nothing is written to the machine's disks.
   `docs/hardware/bringup.md`: how to write the stick (`dd` or Etcher), firmware settings (UEFI on, Secure Boot
   off, legacy USB support on where offered), what to photograph, and the buying check for a low-tier machine
   (knowledge-os.md §4).
6. `arc.bench=1` (ADR-0019): TSC-timed runs of the hot paths that exist (4 KiB and 2 MiB page allocation,
   kmalloc, context switch), printed as one table. Later blocks add their hot path (page fault, read, mmap fault).
   The photographed result goes in `docs/perf/<date>-<machine>.md`.
7. QEMU tests: a forced panic (test-only command line flag) shows the panic markers on serial; the boot report
   markers are present.

**Done when.**
- [ ] QEMU tests pass.
- [ ] **Michael:** both machines boot the stick; photos of the boot report and self-test are posted in the PR.
      Small fixes go in this block; anything bigger becomes a new block in work-blocks.md.
- [ ] The old work computer's maker and model, CPU, RAM, disk controller and Ethernet chip, read from its boot
      report, are recorded in knowledge-os.md §4 (Low row).

**Watch out.**
- Low-tier machine: a USB keyboard works only through the firmware's legacy USB emulation until xHCI (M4-01); a
  laptop's built-in keyboard is usually PS/2 and works now. Nothing in the report may need input.
- Secure Boot must be off. If the old work computer can't turn it off, it can't boot arc_os yet (ADR-0018); report
  that instead of working around it.
- Laptop: no serial port; the screen is the only channel.
- Framebuffers differ (pitch ≠ width × 4, BGR vs RGB). Use the Limine framebuffer masks.

**Notes.**
- From M1-03: the kernel runs x2APIC whenever CPUID has it (Limine is asked to switch every CPU too). It does
  not read the DMAR table's x2APIC opt-out flag; if a test machine misbehaves with x2APIC, that flag is the first
  thing to check. The boot log shows `[APIC] x2APIC mode` / `xAPIC mode` and one `[SMP] CPU n (APIC a) online`
  line per AP. `kprintf` has no lock: only the BSP prints during SMP bring-up.
