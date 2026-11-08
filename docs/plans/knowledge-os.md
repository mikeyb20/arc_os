# arc_os Offline Knowledge Computer — Master Plan

> **Status: APPROVED (2026-11-07).** This file is the roadmap for arc_os. It replaces the phase list in
> `overview.md`, which is kept as history for phases 0–12.
> The build order (work blocks, sizes, status) is in [`work-blocks.md`](work-blocks.md).
> Changing anything under "Locked technical spec" or "Lock-in decisions" needs Michael's explicit OK
> **and** a new or superseding ADR in `docs/decisions/`. Everything else (task lists, order inside a
> milestone) may be refined as work proceeds, as long as the change is recorded here.

## 1. Goal

A standalone x86_64 PC, installed only from a USB key, running arc_os. Local LLMs search a large offline
library (general/Wikipedia, medical, food, repair/building, maps) and help operate the computer. It is
fully functional with no network; a wired network is used only if present. It is a one-time build,
adjusted by hand afterwards, with no internet updates.

Guiding rule: **basic but able to scale.** Nothing outside this plan gets built without a yes from
Michael. Any choice that would force a later rewrite is flagged before code is written on top of it.

## 2. Milestones at a glance

| # | Milestone | Done when |
|---|---|---|
| M0 | Lock-in decisions | ADRs 0001–0019 written; syscalls migrated to the Linux ABI; schemas, dev container, CI and QEMU test harness in place |
| M1 | Compute kernel | A 4-thread AVX2 program runs on 4 CPUs and memory-maps a 10 GB ext2 file with 4 GB RAM in QEMU; arc_os boots to the shell on both real test machines |
| M2 | Toolchain + first LLM | `ask "what is hypothermia"` answers inside arc_os in QEMU (4 CPUs, 4 GB); tokens/sec and RAM budget recorded |
| M3 | Knowledge layer | A medical, a food and a repair question each get a cited answer from the 32 GB pack set; the library is usable with no LLM |
| M4 | Real hardware | M3's test passes on both test machines, with and without a network cable |
| M5 | Packages + installer | A blank machine becomes a working system from one USB key, with no network; the same key runs the full system on a machine whose internal disk stays untouched |

M0–M3 run in QEMU so the LLM answers before real-hardware drivers are written. M1 includes an early
real-hardware boot so hardware surprises show up before the kernel design is built upon.
Pack building runs on a Linux host at build time; the finished computer runs only arc_os.

## 3. Locked technical spec

This section mirrors the project instructions. Each item links to its ADR.

### 3.1 Platform
- x86_64 only for now; RISC-V later through the HAL. CPU floor **x86-64-v3** (AVX2, FMA, BMI2, XSAVE). Scheduler is aware of hybrid P/E cores. ([ADR-0003](../decisions/0003-fpu-xsave.md))
- Boot: Limine. UEFI primary, legacy BIOS secondary. Secure Boot off for now; signing later. The kernel reads only `BootInfo`.
- Dev target: QEMU `q35`, `-smp 4`, `-m 4G` (matches the low tier).

### 3.2 Kernel
- Monolithic, C11 + NASM, higher-half kernel, HHDM; style rules in `CLAUDE.md`.
- SMP: per-CPU run queues, per-CPU syscall stacks, preemptive; written lock-ordering rules.
- Memory: 4 KiB + 2 MiB pages; per-page descriptor array allocated from the PMM at boot (never kmalloc); demand paging; copy-on-write fork; one unified page cache serving `read`/`write` and `mmap`; clean-page eviction; no swap. ([ADR-0004](../decisions/0004-page-cache-and-mmap.md), [ADR-0016](../decisions/0016-kernel-memory-sizing.md))
- FPU: eager XSAVE/XRSTOR, save-area size from CPUID leaf 0xD. ([ADR-0003](../decisions/0003-fpu-xsave.md))
- Interrupts/time: LAPIC + IOAPIC; MSI/MSI-X for PCIe devices; invariant TSC as clocksource; TSC-deadline/LAPIC timer; PIT only for calibration; CMOS RTC for wall time.
- ACPI: tables parsed in-kernel; AML through uACPI (power off, reboot, battery).
- Hardening: SMEP, SMAP, NX/W^X, stack canaries, guard pages. ASLR later.
- Power: tickless idle, MWAIT C-states, Intel HWP/EPP, power profiles (performance/balanced/saver), RAPL energy readout; battery, AC, lid, buttons and backlight via ACPI; every driver has suspend/resume hooks. System suspend itself is later. ([ADR-0017](../decisions/0017-power-management.md))

### 3.3 User-space ABI (Linux-compatible by convention, no Linux code)
- Linux x86_64 syscall numbers, semantics, errno convention and struct layouts; `SYSCALL`/`SYSRET`. ([ADR-0001](../decisions/0001-linux-syscall-abi.md))
- System V process start (argv/envp/auxv incl. `AT_PHDR`, `AT_PAGESZ`, `AT_RANDOM`, `AT_HWCAP`), TLS via FS base, `clone` + `futex` threads, full `rt_sig*` signals. ([ADR-0002](../decisions/0002-process-start-and-tls.md))
- Static ELF64 only (`ET_EXEC` and static-PIE); no dynamic linker for now.
- Devices use Linux interfaces too: `/dev/input/eventN` with Linux `struct input_event`; `/dev/fb0` with fbdev ioctls + `mmap`. ([ADR-0007](../decisions/0007-unified-input-events.md), [ADR-0014](../decisions/0014-userspace-graphics-and-input.md))
- Text is UTF-8 everywhere; console fonts in PSF2 format. ([ADR-0013](../decisions/0013-utf8-everywhere.md))

### 3.4 Toolchains and outside code
- Kernel: `x86_64-elf-gcc`. User space: `x86_64-linux-musl` GCC + libstdc++, static. Pinned versions in a reproducible dev container.
- In-house programs link **libarc**; ported software links **musl**.
- The exact source of every outside component (GCC, binutils, musl, Limine, llama.cpp, SQLite, zstd, uACPI) is archived in Michael's repos and on the install key. No build may need a third-party server.

### 3.5 Storage
- Block layer: async request queue, DMA, interrupt completion, 64-bit LBA, 512e/4Kn. ([ADR-0005](../decisions/0005-async-block-layer.md))
- Drivers: virtio-blk, NVMe, AHCI, USB mass storage (BOT).
- GPT disks: ESP (FAT32, Limine) + system (ext2) + data (ext2); sizes fixed in M5. ([ADR-0008](../decisions/0008-disk-layout-and-filesystem.md))
- ext2 rev 1, 4 KiB blocks, `large_file`, read/write; journaling later. Durable updates = write temp file, `fsync`, `rename`.
- Directory layout: `/system` (OS, read-only) `/apps` `/models` `/packs` `/home` `/var`. ([ADR-0009](../decisions/0009-directory-layout.md))
- Portable mode: the USB key is an ordinary arc_os disk with the same layout. arc_os runs entirely from it, mounts only its own partitions from the disk it booted from, and never writes to a disk it didn't create. ([ADR-0018](../decisions/0018-portable-usb-mode.md))

### 3.6 Devices and network
- In scope: PS/2 keyboard and mouse, xHCI + USB HID + mass storage, NVMe, AHCI, Intel e1000/e1000e (I219), Realtek RTL8111/8168, virtio-blk/net, Limine/UEFI framebuffer. ([ADR-0006](../decisions/0006-driver-model.md))
- Out of scope: Wi-Fi, Bluetooth, GPU acceleration, audio, I2C-HID touchpads, system suspend/hibernate (hooks exist; ADR-0017).
- Network: IPv4, TCP, UDP, DHCP, DNS, AF_UNIX sockets. IPv6 later.

### 3.7 AI and data
- Inference: llama.cpp, CPU only, GGUF models. Apps reach models only through the `arc-llm` service (AF_UNIX socket, OpenAI-style JSON). ([ADR-0012](../decisions/0012-model-service-api.md))
- Model roles chosen by the user: `chat`, `embed`, `router`, `expert:<domain>`. Install refuses models that don't fit RAM.
- Packages: one `.arcpkg` format for programs, models and packs. ([ADR-0010](../decisions/0010-arcpkg-format.md))
- Packs: zstd doc store + chunks + SQLite FTS5 keyword index + per-embedder vectors. Retrieval = keyword first, vector rerank. ([ADR-0011](../decisions/0011-data-contract.md))
- The library works with no LLM. ([ADR-0015](../decisions/0015-library-without-llm.md))
- Storage tiers: 32 GB default, 1 TB max, 1–5 TB add-ons later. Content priority: general, medical, food, repair/building, maps.

## 4. Hardware targets

| Tier | RAM | CPU | Chat model (4-bit, approx.) | Notes |
|---|---|---|---|---|
| Low (floor) | 4 GB | 2–4 cores, AVX2 | 1–2B, ~1–1.5 GB | Small embedder (~100 MB), short context |
| Mid | 8–16 GB | 4–8 cores, AVX2 | 7–8B, ~5 GB | |
| High (design target) | 32–64 GB | 8+ cores, AVX2/AVX-512 | 14B dense, or MoE with ~3B active | MoE suits CPUs: only active experts compute |

Model sizes are approximate and get confirmed by measurement in M2.

**Low-tier reference machine** (Michael, 2026-11-07): a standard used business laptop of the kind sold for about
$150–300 CAD on Facebook Marketplace or Kijiji. If arc_os works there, it works on most Intel machines from about
2015 on. Typical parts: Core i5 of the 6th–10th generation, 8 GB run at 4 GB (`arc.mem=4G`), SATA or NVMe SSD,
Intel xHCI, Intel I219 Ethernet on most models, PS/2 keyboard, PS/2 or I2C touchpad, often a 1366×768 screen, UEFI
with a Secure Boot switch. Examples: Dell Latitude 5490/7490, Lenovo ThinkPad T480, HP EliteBook 840 G5 (parts and
examples are a reading of that market, not checked model by model).

- Buying check: Core i3/i5/i7, not Celeron or Pentium (those usually lack AVX2 and fall below the CPU floor); no
  BIOS password set (it blocks turning Secure Boot off); an Intel or Realtek Ethernet port if a wired network matters.
- Gaps until the compatibility track ([`work-blocks.md`](work-blocks.md) §9): AMD power management, I2C touchpads, USB
  Ethernet adapters, network chips beyond Intel and Realtek.

### Test machines

| Role | Machine | Key hardware | Notes |
|---|---|---|---|
| High | Michael's laptop ("scrubtop") | Intel Core Ultra 7 255H (16 cores, hybrid P/E, AVX2 + AVX-VNNI, no AVX-512), 32 GB, Crucial P3 1 TB NVMe, Intel Ethernet `8086:550a` (I219 family, unverified), Intel xHCI, Intel integrated graphics | Keyboard is PS/2 (works with today's driver). Touchpad is I2C (out of scope); a USB wireless mouse is used. **No serial port.** Secure Boot already off. Wi-Fi `8086:272b` out of scope. |
| Low | Michael's old work computer (owned) | Stands in for the low-tier reference machine above; its exact parts are recorded from the M1-04 boot report, not collected up front. CPU must meet x86-64-v3 (Intel 4th-gen Core or newer). Run at 4 GB (capped in software if it has more) | Its internal disk must stay as it is, so arc_os runs only from the USB key ([ADR-0018](../decisions/0018-portable-usb-mode.md)). Its firmware must allow a USB boot with Secure Boot off; a BIOS password left by the former employer would block that. With no internal disk in use, AHCI writes are tested only in QEMU |

### Storage tiers

| Tier | Contents |
|---|---|
| 32 GB (default) | OS, one small chat model, embedder, curated Wikipedia subset, full medical/food/repair packs, regional maps |
| 1 TB (max) | Everything above plus full Wikipedia with images, larger models, world maps |
| 1–5 TB add-ons (later) | Extra packs installed the same way, no format change |

## 5. Lock-in decisions

Cheap now, expensive later. Each has an ADR in `docs/decisions/`.

| # | Decision | ADR |
|---|---|---|
| 1 | Linux x86_64 syscall ABI (numbers, semantics, structs) | [0001](../decisions/0001-linux-syscall-abi.md) |
| 2 | System V process start, TLS via FS base | [0002](../decisions/0002-process-start-and-tls.md) |
| 3 | Eager XSAVE, CPUID-sized save area | [0003](../decisions/0003-fpu-xsave.md) |
| 4 | Unified page cache, demand paging, mmap, COW, eviction, 2 MiB pages | [0004](../decisions/0004-page-cache-and-mmap.md) |
| 5 | Async block layer with DMA and interrupts | [0005](../decisions/0005-async-block-layer.md) |
| 6 | Driver model: bus registration, match by PCI/USB IDs, MSI-X | [0006](../decisions/0006-driver-model.md) |
| 7 | Unified input events (`/dev/input`, Linux `input_event`) | [0007](../decisions/0007-unified-input-events.md) |
| 8 | GPT + ESP/system/data; ext2 rev 1 read/write | [0008](../decisions/0008-disk-layout-and-filesystem.md) |
| 9 | Directory layout `/system /apps /models /packs /home /var` | [0009](../decisions/0009-directory-layout.md) |
| 10 | One `.arcpkg` package format | [0010](../decisions/0010-arcpkg-format.md) |
| 11 | Data contract: packs, chunks, stable chunk IDs | [0011](../decisions/0011-data-contract.md) |
| 12 | Models only via the `arc-llm` service | [0012](../decisions/0012-model-service-api.md) |
| 13 | UTF-8 everywhere, PSF2 fonts | [0013](../decisions/0013-utf8-everywhere.md) |
| 14 | Graphics and input reachable from user space | [0014](../decisions/0014-userspace-graphics-and-input.md) |
| 15 | Library works without the LLM | [0015](../decisions/0015-library-without-llm.md) |
| 16 | Per-page metadata from the PMM at boot | [0016](../decisions/0016-kernel-memory-sizing.md) |
| 17 | Power management first-class: tickless, C-states, HWP, profiles, battery, driver suspend/resume hooks | [0017](../decisions/0017-power-management.md) |
| 18 | Portable mode: runs entirely from the USB key; own partition types; never writes to host disks | [0018](../decisions/0018-portable-usb-mode.md) |
| 19 | Efficiency: cost lines per block, hot-path rules, CI budgets (counts and sizes), slab allocator, 32-byte page descriptors, page cache ready for large pages | [0019](../decisions/0019-efficiency-budgets-and-allocators.md) |
| 20 | Code hygiene: CI ratchets on duplicates, copied helpers, x86 includes outside the HAL, long functions and untracked TODOs; one home per helper; block review before every PR | [0020](../decisions/0020-code-hygiene-ratchets.md) |

## 6. Data design

Every pack keeps full documents plus a layer of small text **chunks** with permanent IDs. Every index and
every model works off chunk IDs, so a better embedder, a new index or an MoE expert can be added later
without reprocessing the library. Details and exact formats: [ADR-0011](../decisions/0011-data-contract.md).

```
/packs/<pack-id>/
  manifest.json        name, version, schema_version, category, priority, size, license, source, checksums, provides (schemas/pack.schema.json)
  content/             full documents in our own zstd-compressed store keyed by document ID (maps: pre-rendered tiles)
  chunks/              plain-text passages of ~300-500 tokens, one fixed-size index record each
  fts/                 SQLite FTS5 keyword (BM25) index over the chunks
  vectors/<embedder>/  one embedding per chunk, int8 or fp16, same order as chunks
```

- **Chunk ID** = `pack:document:chunk-number`, stable within a pack version; every index records the pack version it was built from. Each chunk records title, section path, domain tags and its location in the source document.
- **Memory-mapped arrays everywhere.** No parsing at question time.
- **Several embedders can coexist**, one `vectors/<embedder>/` folder each.
- **Domain tags** are the hook for routing to MoE/expert models.
- **Heavy work happens at build time** on a Linux host (extraction, chunking, embeddings). The finished computer only reads packs.
- **Retriever interface:** `search(query, packs, k)` → chunk IDs + scores. FTS5 keyword search finds candidates; vectors rerank the top ~200 by chunk ID. A true vector index (IVF) can be added later behind the same call.
- Images are kept for medical and repair packs.

## 7. LLM design

- **Model package** = GGUF weights + manifest (name, version, role, parameter count, quantization, RAM needed, context length, chat template, license, checksum, embedder ID for `embed` models).
- **Roles** set separately by the user: `chat`, `embed`, `router` (optional), `expert:<domain>` (later). A config file records which installed model fills each role.
- **Fit check** before install: refuse or warn when the model's RAM need exceeds the machine.
- **Upgrade** = install beside the old model, switch the role, keep the old one until removed. Rollback = switch back.
- **`arc-llm` service** exposes `generate`, `embed`, `tokenize`, `list_models` on an AF_UNIX socket using OpenAI-style JSON.
- **Embedder lock:** a pack's vectors only work with the embedder that made them; switching embedders requires vectors for the new one.
- **Assistant tools** (versioned list): `search_packs`, `open_source`, `read_file`, `list_dir`, `run_command` (asks the user first). The shell gets an `ask` command.
- **Safety:** answers come only from retrieved sources, always cited; "not found in library" when retrieval is empty.

## 8. Milestones in detail

Every milestone ends with an automated test (QEMU or real hardware) checked over serial or the log
channel, on top of host-side unit tests. The work is split into PR-sized blocks in
[`work-blocks.md`](work-blocks.md). M0 and M1 have full specs in `docs/plans/blocks/`; each later milestone
starts with a planning block that writes its specs.

### M0 — Lock-in decisions
- [x] ADRs 0001–0020
- [x] Syscall inventory: run llama.cpp, SQLite and our test programs under `strace` on Linux; record the exact syscall list M1/M2 must provide
- [x] Migrate syscalls to Linux x86_64 numbers and semantics in one pass with libarc, userland and tests together
- [x] JSON schemas for pack, model and `.arcpkg` manifests, each with `schema_version` (M0-07, `schemas/`)
- [x] Reproducible dev container with pinned versions of both toolchains; GitHub Actions running unit and QEMU tests
- [x] QEMU integration-test harness (boot, run a program, check serial output)
- [x] Linux process start (System V stack, auxv, TLS): stock static musl programs run in QEMU (M0-06)
- [x] Code hygiene checks in CI: counts of duplicates, copied helpers, x86 includes outside the HAL, long functions and untracked TODOs never rise (ADR-0020)
- [x] Archive the exact source of every outside component (§3.4) in repos Michael owns
- [x] M1 implementation plan: PR-sized tasks, dependency order, SMP lock-ordering rules ([`work-blocks.md`](work-blocks.md) §4 and §7, `docs/plans/blocks/M1-*`)

### M1 — Compute kernel
- [ ] Per-page descriptor array from the PMM at boot (ADR-0016)
- [x] Eager XSAVE/XRSTOR, CPUID-sized save area; SSE/AVX enabled for user space; kernel built with `-mgeneral-regs-only` (M1-01; `kernel.no_simd` checks it)
- [ ] Per-CPU SYSCALL kernel stack fix; per-CPU run queues; x2APIC mode; up to 256 CPUs
- [ ] Hybrid CPU awareness: core type from CPUID; inference threads can pin to P-cores (core type per CPU since M1-01)
- [ ] Threads: `clone`, `futex`, `arch_prctl`, `set_tid_address`, `exit_group`, `sched_setaffinity`, `rt_sigprocmask` (single-thread `arch_prctl`, `set_tid_address`, `exit_group` since M0-06)
- [ ] Memory: COW fork, demand paging, `mmap`/`munmap`/`mprotect`/`madvise`, page cache shared with `read()`, clean-page eviction, 2 MiB pages
- [ ] Time: invariant TSC clocksource, TSC-deadline timer, `clock_gettime`, `nanosleep`, CMOS RTC
- [ ] Hardening: SMEP, SMAP, NX enforced
- [ ] Power base: tickless idle, MWAIT idle states, Intel HWP/EPP, RAPL energy counters (ADR-0017)
- [ ] Block layer: async request queue with interrupts; virtio-blk moved onto it
- [ ] VFS inode refactor with 64-bit offsets; GPT parsing; ext2 rev 1 read/write with large files; one data partition mounted as `/apps /models /packs /home /var`; system and data mounted from the disk Limine booted from (ADR-0018)
- [ ] AF_UNIX sockets and poll/select; driver model with MSI/MSI-X
- [ ] Real-hardware debug channel: framebuffer log with scrollback, panic screen and a boot report to photograph (the laptop has no serial port). A log file needs a disk driver, so it moves to M4; UDP log later
- [ ] **Done when:** a 4-thread AVX2 test program runs on 4 CPUs and memory-maps a 10 GB ext2 file with 4 GB RAM in QEMU, and arc_os boots to the shell on both real test machines

### M2 — Toolchain and first LLM
- [ ] Port musl (static); stock `x86_64-linux-musl` GCC with libstdc++
- [ ] C++ test program: exceptions, threads, `std::filesystem`
- [ ] Port SQLite and llama.cpp (CPU build)
- [ ] `arc-llm` service + `ask` command
- [ ] **Done when:** `ask "what is hypothermia"` answers in QEMU (4 CPUs, 4 GB); tokens/sec and a RAM budget (OS, chat model, embedder, page cache) recorded for the low tier

### M3 — Knowledge layer
- [ ] Host pipeline: source ZIM → extract → chunk → FTS5 index → embeddings → manifest → `.arcpkg`
- [ ] Tier selector: packs in priority order to fit 32 GB or 1 TB, using measured index and vector sizes
- [ ] Document store (zstd) so citations open the full source article; images kept for medical/repair
- [ ] UTF-8 console with a PSF2 font covering Latin + common symbols (°, µ, ±)
- [ ] Retriever library: keyword search, vector rerank, top results
- [ ] Library reader with no LLM: search, open article, follow links, bookmarks
- [ ] Image viewer on the framebuffer
- [ ] Assistant (text UI): numbered citations, tool calls (§7)
- [ ] Maps, part 1: place-name search
- [ ] Response-time target for the low tier set and measured
- [ ] **Done when:** a medical, a food and a repair question each get a cited answer from the 32 GB pack set, and the same library is searchable and readable with `arc-llm` stopped

### M4 — Real hardware
- [ ] USB: xHCI, HID keyboard and mouse, mass storage (BOT). Mass storage is essential: portable mode depends on it (ADR-0018)
- [ ] Storage: NVMe (laptop), AHCI (tested in QEMU; neither test machine uses SATA now)
- [ ] Input: PS/2 mouse; all input through `/dev/input`
- [ ] Network (optional at runtime): Intel e1000e/I219 first, then Realtek RTL8111/8168; DHCP, DNS
- [ ] Power: ACPI power-off and reboot via uACPI; battery %, AC adapter, lid and power button; low-battery warning and clean shutdown at critical level; power profiles wired to EPP and `arc-llm` thread count; screen blanking and backlight control; NVMe APST; `power` command showing battery, profile and watts
- [ ] Maps, part 2: framebuffer map viewer with pre-rendered tiles; pan and zoom by keyboard or mouse (I2C touchpads are out of scope)
- [ ] Kernel log kept in a file under `/var/log` (moved from M1: needs the NVMe/AHCI drivers)
- [ ] **Done when:** M3's test passes on both test machines, with and without a network cable; idle and inference power are recorded per profile on both machines

### M5 — Packages and installer
- [ ] `arcpkg`: install, remove, list, verify, switch model role; offline from USB or disk
- [ ] Installer: detect disks, create GPT layout, format, copy system, pick storage tier by disk size and models by RAM (`mke2fs`/`e2fsck` ported from e2fsprogs; the ESP is written from a prebuilt FAT32 image)
- [ ] Host script that builds the USB key: the portable layout (ADR-0018) written straight onto the stick and sized to fill it (Limine, UEFI + legacy BIOS)
- [ ] Host tool that wraps any GGUF into an `.arcpkg`
- [ ] Full offline copy of arc_os source, archived outside sources and toolchains on the key
- [ ] Recovery: the install key doubles as a repair key; `arcpkg verify` restores damaged packs/models from it
- [ ] **Done when:** a blank machine becomes a working system from one USB key with no network, and the same key runs the full system on the USB-only test machine without touching its internal disk

## 9. Out of scope (first build)

Wi-Fi, Bluetooth, GPU acceleration, audio, I2C-HID touchpads, a desktop/window manager, multi-user,
system suspend/hibernate (driver hooks exist), internet updates, dynamic linking, IPv6, ASLR, Secure Boot signing, ARM.
Static Linux programs may run thanks to the ABI, but that is not a goal.

## 10. Risks

| Risk | Impact | Plan |
|---|---|---|
| Small models make things up, including medical answers | Safety | Answers only from retrieved sources, always cited; "not found in library" when retrieval is empty; source one keystroke away |
| Many laptops have only Wi-Fi | No network on those machines | The system works offline anyway; wired Ethernet where the machine has a port. USB Ethernet adapters need their own driver (compatibility track) |
| Secure Boot blocks the unsigned bootloader | Won't boot | Install docs say to turn it off. Where it can't be turned off, the known path is a Microsoft-signed shim plus our own enrolled key (ADR-0018); a separate decision when needed |
| USB keys are slow at random reads and wear out with writes | Slow search; a worn key | Read-only system partition, no access-time updates, batched writeback (ADR-0018); recommend a USB 3 stick with good random reads or a USB SSD; measured in M4-14 |
| ext2 has no journal | Power loss can damage files being written | Write temp, verify checksum, `fsync`, `rename`; journal later |
| Real-hardware driver variety | M4 drags on | Support the two test machines first; early boot check in M1 |
| Running on battery or solar | Shorter uptime | Power is first-class (ADR-0017): tickless idle, profiles, critical-battery shutdown, measured watts |
| No serial port on the laptop | Hard to debug real-hardware boots | Framebuffer log, panic screen and boot report in M1; log file on disk in M4 |
| Embedding a large library is slow | Pack build time | Strongest host available; packs may ship keyword-only and add vectors later |
| Content and model licenses | Matters if copies are shared | Every manifest records license and source |
| arc_os repo is public | Plans and manifests are visible | Fine for code; private material goes in a separate private repo |

## 11. Review history

- **2026-11-08 — Maintainability review** (ADR-0020, approved by Michael). Found duplicated kernel helpers (HHDM address conversion), 44 direct x86 includes in 8 subsystems despite the HAL rule, no format check and no shared review checklist. Added CI ratchets that freeze those counts (new block M0-09), a block-review skill run before every PR, one home per helper, and HAL include moves inside the M1 blocks that already rewrite those call sites. No standalone refactor.
- **2026-11-08 — Efficiency review** (ADR-0019, approved by Michael). Found an unlocked kernel heap reachable from an interrupt (fixed), no object allocator in the plan (new block M1-02b), a 64-byte vs 32-byte page-descriptor conflict (32 kept), and a 4 KiB-only page-cache design (API made order-agnostic). Added cost lines, hot-path rules and CI budgets on counts and sizes (new block M0-08); real timing stays on hardware checkpoints.
- **2026-11-07 — Architect review.** Added per-page metadata and eviction; keyword-first retrieval with vector rerank; own doc store instead of a libzim port; early real-hardware boot and MSI-X in M1; Linux semantics (not just numbers); low tier set to 1–2B models.
- **2026-11-07 — Six-reviewer pass** (junior coder, senior coder, product user, tech enthusiast, OS expert, off-grid operator). Added lock-ins 13–16; syscall inventory, dev container, CI; library reader and images; GGUF wrapping tool; battery awareness; verify-and-repair.
- **2026-11-07 — Final read-over before committing.** Fixed: battery display contradicted "battery management out of scope" (now only suspend/hibernate is out); COW fork, TSC, SMEP/SMAP and PSF2 fonts from the approved spec were missing from milestone tasks; uACPI missing from the archive list; M1 done-test wording aligned.
- **2026-11-07 — Power management added** at Michael's request (ADR-0017). Battery and power saving are in scope; system suspend stays later because the laptop likely needs S0ix.
- **2026-11-07 — Work-block plan** ([`work-blocks.md`](work-blocks.md)): 70 blocks, full specs for M0 and M1, session rules to keep compute down, lock order. Code inspection added to M1: `-mgeneral-regs-only` (live bug), x2APIC and 256 CPUs, 64-bit VFS offsets, poll/select, one data partition for five directories. The M1 log file moved to M4 (no disk driver on real hardware before then). M5 installer needs an e2fsprogs port. Proposed: defer the Realtek driver (neither test machine has one); Michael kept it on 2026-11-08.
- **2026-11-07 — Portable mode added at Michael's request** (ADR-0018). The low-tier machine is his old work computer, which can't keep installs, so arc_os runs entirely from the USB key like Tails. The key uses the normal disk layout; the kernel mounts from the disk it booted from; host disks are never written. Built inside existing blocks (M1-02, M1-10, M1-13, M1-15, M4-04, M5); more drivers for other people's machines come in a later compatibility track.
- **2026-11-07 — Low-tier reference machine** set by Michael: a standard used business laptop (about $150–300 CAD); the 4 GB floor stays. The old work computer stands in for that class, and its parts come from the M1-04 boot report instead of being collected up front. Follow-ups: buying check, map panning by keyboard, 1366×768 as the smallest screen, and the USB Ethernet mitigation corrected (no driver for it is planned).
- **2026-11-07 — Decisions by Michael:** Linux syscall ABI (yes); test machines (laptop + used ThinkCentre Tiny i3); Secure Boot off; project instructions approved.
