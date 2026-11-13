# Bring-up USB stick (M1-04)

The stick boots arc_os on a real machine and shows what the kernel found, on screen, so hardware surprises show
up early. Everything runs from RAM ([ADR-0018](../decisions/0018-portable-usb-mode.md), RAM mode): the kernel and
programs are loaded by Limine, no USB or disk driver is used, and nothing is written to the machine's disks.

## 1. Get the image

**Download (usual way).** CI publishes the image of the newest master commit that passed its tests as the
pre-release [`bringup-latest`](https://github.com/mikeyb20/arc_os/releases/tag/bringup-latest). No login is needed:

- [`arc_os-bringup.iso`](https://github.com/mikeyb20/arc_os/releases/download/bringup-latest/arc_os-bringup.iso)
- [`arc_os-bringup.iso.sha256`](https://github.com/mikeyb20/arc_os/releases/download/bringup-latest/arc_os-bringup.iso.sha256)
  (check with `sha256sum -c arc_os-bringup.iso.sha256` in the download folder, or compare with the hash in the
  release notes)

The release notes name the commit it was built from; write that short hash next to the photos. The release is
replaced after every push to master, so download it again before each boot.

**Build it yourself** (any commit, or offline):

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake && cmake --build build
cmake --build build --target iso-bringup        # or: tools/make-iso.sh -B
```

The result is `build/arc_os-bringup.iso`, a hybrid image: the same file boots as a CD or, written raw, as a USB
stick, under legacy BIOS and UEFI. It has a boot menu (10 s timeout):

| Entry | Command line | What it does |
|---|---|---|
| **boot report, self-test and benchmark** (default) | `arc.selftest=1 arc.bench=1` | Report, then the self-test, then the benchmark table |
| boot report | `arc.report=1` | Report only (fastest) |
| boot report as QR codes (with self-test and benchmark) | `arc.qr=1 arc.selftest=1 arc.bench=1` | The first entry's text as QR codes, one per screen (§4a) |
| kernel log as QR codes | `arc.qr=log` | The newest 8 KiB of the kernel log as QR codes (§4a) |
| normal boot | (none) | The usual boot to the login prompt |

The diagnostic entries never start `init`: the report pages repeat forever until the machine is switched off.

## 2. Write the stick

Any stick of 64 MB or more. **Everything on it is erased.**

- **Linux / macOS:** find the stick's device (`lsblk` on Linux, `diskutil list` on macOS; the whole disk such as
  `/dev/sdb` or `/dev/disk4`, not a partition), then
  `sudo dd if=build/arc_os-bringup.iso of=/dev/sdX bs=4M conv=fsync status=progress`
  (macOS: `of=/dev/rdiskN bs=4m`, after `diskutil unmountDisk /dev/diskN`).
- **Windows / any OS:** balenaEtcher: "Flash from file", pick the ISO, pick the stick, "Flash". Rufus also works
  in "DD image" mode (not "ISO image" mode).

## 3. Firmware settings

Enter the firmware setup (usually F2, F1, Del or Enter then F1 on business laptops) and set:

- **UEFI boot on.** Legacy/CSM may stay on; the stick boots either way.
- **Secure Boot off.** arc_os is not signed. If the firmware cannot turn it off (for example a supervisor
  password left by a former owner), the machine cannot boot arc_os yet: note that and stop; do not work around it.
- **Legacy USB support on** where offered. Until the xHCI driver (M4-01), a USB keyboard works only through it.
  A laptop's built-in keyboard is usually PS/2 and works without it. Nothing in the report needs a key.
- Boot from the stick with the one-time boot menu (often F12, F9 or Esc), not by changing the boot order, so the
  internal disk stays the default.

## 4. What to photograph

The laptop has no serial port, so the screen is the only output.

1. **Every page of the boot report.** Pages turn every 10 s, or at once on any key, and repeat. Each page has a
   blue banner with its number ("page 1/2"). Hold the camera square to the screen; the text is 2× size on
   screens 2400×1350 and larger.
2. **The self-test lines** (green PASS, red FAIL) and the **BENCH** table. They are on the last page.
3. **A panic screen**, if one appears (red, yellow banner "arc_os KERNEL PANIC - scan the code or photograph
   this screen"): scan its QR code (§4a), or photograph the whole screen so the registers and the backtrace are
   readable.
4. If the screen stays black after the Limine menu: a photo of the menu and a note of how long you waited.

Post the photos in the M1-04 pull request. Benchmark results go in `docs/perf/<date>-<machine>.md`
([template](../perf/README.md)); the low-tier machine's parts go in [knowledge-os.md](../plans/knowledge-os.md) §4.

### 4a. Scanning the QR codes instead (M1-04c)

The two QR entries show the same text as codes, so it arrives exactly, with nothing to retype.

1. Pick "boot report as QR codes" (or "kernel log as QR codes") in the boot menu.
2. Each screen is one code, black on white, with a line under it: `arc_os report  page 1/4  crc 1a2b3c4d`.
   Pages turn every 10 s, or at once on any key, and repeat.
3. Point the phone's camera app at the code. It shows the text as a preview: tap the preview, then **Copy**
   (iPhone: "Copy"; Android camera or Google Lens: "Copy text"). Don't open anything.
4. Paste each page into the thread in order. Every page starts `ARC1 <build> report <n>/<N> <crc>`; the last one
   ends with `all <crc>`. The reading thread checks both CRCs, so a page that was misread or missed shows up.
5. A panic screen carries one code at the top right (kind `panic`): reason, CPU, registers and backtrace. The
   log text wraps to the left of the code, so every register stays readable in a photo too.

**Is it safe to scan?** A QR code is only text; the risk is what a phone does with it, such as open a link,
join a Wi-Fi network or call a number. arc_os's codes always start with `ARC1` and can never hold a link (the
kernel writes any `://` as `:\/\/` and any byte outside plain ASCII as `\xNN`), so the phone shows text and
nothing else. The habit that keeps any code safe: read the preview first, and never tap a code that offers to
open a website, join a network or call someone.

If the screen is too small for a code (under 4 pixels per module), the kernel says so on the log and shows the
text pages instead.

### Reading the report

| Line | What to check |
|---|---|
| boot report, build | The git commit the image was built from (`-dirty`: built with uncommitted changes) |
| Machine, Firmware | Maker and model (SMBIOS type 1); UEFI vs legacy BIOS |
| CPU, CPU id, Features | AVX2 FMA BMI2 XSAVE must be there (the x86-64-v3 floor); TSC-DEADLINE and ARAT (APIC timer keeps running in deep C-states) matter for M1-06, HWP and EPP for M1-21 power profiles |
| CPUs | "N online; MADT lists N": every CPU came up; P/E core counts on hybrid CPUs |
| APIC | xAPIC or x2APIC; "DMAR x2APIC opt-out: SET" means the firmware asked for xAPIC, which the kernel does not act on yet: if that machine misbehaves, this is the first suspect |
| TSC | kHz and its source: `cpuid` (CPUID 0x15), `pit` (measured) or `base` (CPUID 0x16); "invariant" should be there, not "NOT invariant" |
| Memory | Usable RAM, what is reserved, and how much is free after boot |
| Display | Resolution, pixel format, text size |
| ACPI | Table names: the DMAR, HPET, MCFG and FACP lines matter for later blocks |
| PCI | Every device with its class and driver: the disk controller (class 0106 SATA or 0108 NVMe) and Ethernet (0200) chips are the ones later blocks need |
| SELFTEST | cpus, avx2 (on every CPU), memory (256 MB pattern), tsc (against the PIT), timer (PIT interrupt rate) |

A `tsc` FAIL that says "PIT channel 2 never finished" means the machine has no working legacy PIT (common on
newer laptops); the TSC then comes from CPUID and the `timer` line shows whether interrupts arrive at all.

## 5. Low-tier buying check

From [knowledge-os.md](../plans/knowledge-os.md) §4, before buying a used machine for the low tier:

- Core i3/i5/i7 of the 6th generation or newer (4th generation is the hard floor). Not Celeron or Pentium:
  those usually lack AVX2.
- No BIOS/supervisor password set: it blocks turning Secure Boot off.
- Intel or Realtek Ethernet if a wired network matters.
- 8 GB is fine; arc_os can be capped to 4 GB with `arc.mem=4G` (press `E` on a Limine menu entry to add it).
- If the seller allows it, boot this stick before paying: the CPU line and the self-test answer most questions.
