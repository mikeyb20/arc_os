# Work blocks — the build order for arc_os

> **Status: PROPOSED (2026-11-07)** for Michael's review. Once merged, this file is the build order. It implements
> the milestones in [`knowledge-os.md`](knowledge-os.md) §8 and adds no scope. Block status lives in the tables in
> §4; each block's PR updates its own row.

## 1. Summary

<!-- Totals computed by script from the §4 tables on 2026-11-08. Recompute when blocks change. -->

| Milestone | Blocks | Session units | With allowance |
|---|---|---|---|
| M0 Lock-in decisions | 10 | 7.5 | 9.8 |
| M1 Compute kernel | 23 | 29.0 | 41.7 |
| M2 Toolchain + first LLM | 7 | 7.0 | 9.4 |
| M3 Knowledge layer | 12 | 12.0 | 16.1 |
| M4 Real hardware | 15 | 16.0 | 21.1 |
| M5 Packages + installer | 7 | 7.5 | 9.8 |
| **Total** | **74** | **79.0** | **107.9** |

- **74 blocks**: 12 small, 51 medium, 11 large. The 35 deep (D) blocks carry
  43.5 of the 79.0 session units.
- **Session units:** S = 0.5, M = 1, L = 2. "With allowance" adds debugging and rework: D blocks × 1.5, R blocks
  × 1.2. That allowance is an estimate, not a measurement; recheck it after M1, the first milestone with real
  numbers. Budget about **108 sessions**.
- **M1 is the big one:** 23 blocks and 29 units, 17 of them deep. It gives the kernel virtual memory, SMP, threads,
  a page cache and ext2.
- **Critical path:** 35 blocks, 40 units (§5). Running lanes in parallel shortens the calendar, not the compute.

## 2. How blocks work

- **One block = one session = one PR.** An L block takes two sessions and still lands as one PR, with a handoff
  in its Notes in between.
- **Size** (changed lines, tests included): **S** under ~300, half a session · **M** ~300–800, one session ·
  **L** ~800–1500, two sessions.
- **Tier:** **R** = routine; a faster, cheaper model is enough and Michael skims the PR. **D** = deep; use the
  strongest model, and Michael reads the header/interface diff and the tests.
- **Lane:** **K** kernel · **U** user space and ports · **C** content pipeline on a Linux host · **I** infra and
  integration · **H** needs Michael with real hardware · **P** planning.
- **Specs:** M0 and M1 have full specs in [`blocks/`](blocks/). M2–M5 get theirs from a planning block
  (`M2-00` … `M5-00`) run just before the milestone, because they depend on what M1 and M2 measure.
- **Status values:** `todo` · `doing` · `review` (PR open, number in the row) · `done`.

## 3. Session protocol (the compute rules)

Start every block with `/os/implement-block <ID>`. These rules are what keep sessions cheap:

1. **Read only what the block lists.** The block file (including its Notes) and its "Read first" files. No
   repo-wide exploration, no `overview.md`, no ADRs the block doesn't cite, no other blocks.
2. **Don't re-plan.** The spec is decided. If a step is wrong, fix the spec in the same PR and say why. If a
   locked decision is in the way, stop and ask Michael on a decision card with a default.
3. **Right-size the model.** Tier R blocks run on the faster model; tier D on the strongest. No subagents or
   workflows unless the block says so.
4. **Test cheapest first.** Host unit tests (seconds) → the one QEMU test for what changed
   (`ctest -L qemu -R <name>`) → the full QEMU suite once, before the PR. CI runs everything; don't repeat a full
   run locally after CI is green.
5. **Never rebuild toolchains or re-download sources.** They come from the archive (M0-01, M0-03). A broken
   environment gets fixed once in the setup script, not worked around per session.
6. **Logs by marker or tail.** QEMU serial output goes to a file; read the `ARC_TEST` lines and the last lines on
   failure. Never paste whole logs, binaries or generated files into the session.
7. **Hand off in Notes.** At the end of a block, or when a session runs out of room: commit, then write 5–10 lines
   in the block's Notes (done, decisions, surprises, exact next step) and push. `scratchpad*.md` is gitignored and
   cloud sessions are thrown away, so Notes are the only handoff that survives.
8. **Keep later specs true.** If a block changes an interface a later block names, update that block's spec in the
   same PR.
9. **Stay in scope.** Anything outside the block becomes a line in Notes, not code. If a block passes 1.5× its
   size, stop, hand off, and split it.
10. **Batch the hardware.** Real-hardware checks happen at four checkpoints (M1-04, M1-22, M4-14, M5-06), not after
    every change.
11. **Ask once, early, with a default.** Questions to Michael go on a decision card at the start of a block; work
    continues on the default while he decides.
12. **Review effort follows the tier.** R: CI green plus a skim. D: interfaces and tests read closely.
    `/validate/review` at low or medium effort for D blocks only.
13. **Run the block review before the PR.** The `block-review` skill (`.claude/skills/block-review/`) is the same
    checklist every block runs: scope, reuse, boundaries, §7 rules, tests, hygiene counts, docs. Its result goes in
    the PR body (ADR-0020).

## 4. Block list

### M0 — Lock-in decisions

| ID | Block | Size | Tier | Lane | Needs | Done when | Status |
|---|---|---|---|---|---|---|---|
| M0-01 | Dev environment, pinned toolchain, CI | M | R | I | — | CI builds and runs host tests; a fresh cloud session has every tool | done |
| M0-02 | QEMU test harness | M | R | I | M0-01 | `ctest -L qemu` passes in the cloud and in CI; failures named | done |
| M0-03 | Archive outside sources and test models | M | R | I | M0-01 | Every component pinned by SHA-256 and fetched from Michael's GitHub | done |
| M0-04 | Syscall inventory | S | R | U | M0-03 | Exact syscalls, ioctls and files used by musl, C++, SQLite, zstd, llama.cpp | done |
| M0-05 | Move syscalls to Linux numbers | M | D | K | M0-02, M0-04 | No old numbers left; shell and coreutils pass | done |
| M0-06 | Linux process start + musl hello | M | D | K | M0-05 | Programs built with stock `musl-gcc -static` run in QEMU | done |
| M0-07 | Manifest schemas | S | R | C | — | Schemas and examples validated in CI | todo |
| M0-08 | Budget checks in the QEMU harness | S | R | I | M0-02 | A size or count past its limit fails CI, named (ADR-0019) | review (#11) |
| M0-09 | Code hygiene checks | S | R | I | M0-08 | Duplicates, copied helpers, x86 includes outside arch, long functions and untracked TODOs can't grow (ADR-0020) | todo |
| M0-10 | Free reaped processes | S | D | K | M0-09 | `pages_leaked` and `heap_leaked_bytes` budgets are 0 on every QEMU suite | todo |

### M1 — Compute kernel

| ID | Block | Size | Tier | Lane | Needs | Done when | Status |
|---|---|---|---|---|---|---|---|
| M1-01 | FPU state, CPU floor, Linux signal frames | M | D | K | M0-06 | AVX2 state survives switches and signals; no SIMD in the kernel | todo |
| M1-02 | Page descriptors + buddy allocator | M | D | K | M0-02 | Descriptor array from the PMM; 2 MiB blocks available | todo |
| M1-02b | Slab allocator | M | D | K | M1-02 | kmalloc/kfree O(1) from slab caches; first-fit heap gone (ADR-0019) | todo |
| M1-03 | Per-CPU entry paths + x2APIC | M | D | K | M0-06 | `-smp 4` in xAPIC and x2APIC modes; no global syscall stack | todo |
| M1-04 | Hardware bring-up kit + first hardware boot | M | R | K+H | M1-01, M1-02, M1-03 | Both machines boot from USB; boot report photographed | todo |
| M1-05 | SMP scheduler | L | D | K | M1-03, M1-02b | Threads run on all CPUs; lock order checked at runtime | todo |
| M1-06 | Clocks, timers, tickless idle | M | D | K | M1-05 | `clock_gettime`, `nanosleep`; idle CPUs get no timer interrupts | todo |
| M1-07 | VMAs, demand paging, anonymous mmap | L | D | K | M1-02, M1-05 | musl malloc works; bad pointers give `-EFAULT`; TLB shootdown | todo |
| M1-08 | Copy-on-write fork + 2 MiB pages | M | D | K | M1-07 | COW fork; huge pages for large anonymous memory | todo |
| M1-09 | Driver model, MSI-X, DMA helpers | M | D | K | M1-05 | virtio on the bus model with MSI-X | todo |
| M1-10 | Async block layer + GPT | M | D | K | M1-09 | Many requests in flight; partitions found by label | todo |
| M1-11 | VFS rework: inodes, 64-bit offsets | L | D | K | M0-06 | Files past 4 GiB; every filesystem converted | todo |
| M1-12 | Page cache + file mmap | L | D | K | M1-07, M1-10, M1-11 | `read`/`write` and `mmap` always agree | todo |
| M1-13 | ext2 read + mount by label | M | R | K | M1-10, M1-11, M1-12 | Reads `mke2fs` images incl. 5 GiB sparse files | todo |
| M1-14 | Memory reclaim + writeback | M | D | K | M1-12, M1-13 | 2 GiB file in 512 MB RAM; no flusher timer when clean | todo |
| M1-15 | ext2 write | L | D | K | M1-13, M1-14 | `e2fsck`-clean after every scenario | todo |
| M1-16 | Threads I: clone, thread groups, futex | L | D | K | M1-01, M1-05, M1-07 | musl pthread create, join and mutex | todo |
| M1-17 | Threads II: futex, signals, affinity | M | D | K | M1-06, M1-16 | Timed waits, thread-directed signals, CPU pinning | todo |
| M1-18 | AF_UNIX sockets + poll/select | M | R | K | M1-06, M1-11 | Echo server over a UNIX socket; poll with timeouts | todo |
| M1-19 | Hardening: SMEP, SMAP, NX, W^X | M | D | K | M1-07 | All suites pass with SMEP/SMAP/NX on | todo |
| M1-20 | Syscall gap fill | M | R | K | M1-11, M1-17, M1-18 | Every inventory syscall done, or `-ENOSYS` with a reason | todo |
| M1-21 | Power base: idle states, HWP, RAPL | M | R | K | M1-06 | MWAIT/HLT idle; EPP by profile; energy readout | todo |
| M1-22 | M1 done-test + second hardware boot | M | R | K+H | M1-04, M1-08, M1-15, M1-19, M1-20, M1-21 | 10 GB ext2 mmap test passes; both machines reach the shell | todo |

### M2 — Toolchain and first LLM (specs from M2-00)

| ID | Block | Size | Tier | Lane | Needs | Done when | Status |
|---|---|---|---|---|---|---|---|
| M2-00 | Plan M2 | S | D | P | M1-22 | M2 block specs written | todo |
| M2-01 | musl cross toolchain, ports layout, C++ test | M | R | U | M2-00, M0-03 | GCC + libstdc++ for `x86_64-linux-musl` archived; C++ test runs in QEMU | todo |
| M2-02 | Port zstd + SQLite (FTS5) | M | R | U | M2-01 | Round-trip and FTS5 query tests pass in QEMU | todo |
| M2-03 | Port llama.cpp (CPU, static, x86-64-v3) | M | D | U | M2-01 | Greedy output in QEMU matches the same build on Linux | todo |
| M2-04 | `arc-llm` service | L | D | U | M2-03 | generate, embed, tokenize, list_models over AF_UNIX; roles; fit check | todo |
| M2-05 | `ask` command + M2 done-test | S | R | U | M2-04 | `ask "what is hypothermia"` answers in QEMU (4 CPUs, 4 GB) | todo |
| M2-06 | Measure on the laptop, shortlist models | M | R | H | M2-05 | Tokens/s and RAM per tier recorded; mmap vs 2 MiB anonymous loading compared and chosen (ADR-0019); plan §4 updated | todo |

### M3 — Knowledge layer (specs from M3-00)

M3-00 sets the smallest supported screen at 1366×768, common on the low-tier reference laptops: the reader,
assistant and viewers lay out from the framebuffer size, never a fixed one.

| ID | Block | Size | Tier | Lane | Needs | Done when | Status |
|---|---|---|---|---|---|---|---|
| M3-00 | Plan M3 | S | D | P | M2-06 | M3 block specs written | todo |
| M3-01 | Content selection, licences, size budget | M | R | C | M3-00, M0-07 | Source list per category and tier approved by Michael | todo |
| M3-02 | `libarcpack`: doc store, chunks, manifests | M | D | U | M3-00, M2-02 | One library reads packs on Linux and on arc_os | todo |
| M3-03 | ZIM → doc store + chunks (host) | M | R | C | M3-01, M3-02 | Medical pack built with stable chunk IDs | todo |
| M3-04 | FTS5, embeddings, tier selector, `.arcpkg` (host) | M | R | C | M3-03 | 32 GB pack set built and sized | todo |
| M3-05 | UTF-8 console, PSF2, `/dev/input`, `/dev/fb0` | M | D | K | M3-00 | fbdev `mmap` and `input_event` work from user space | todo |
| M3-06 | Retriever library | M | D | U | M3-02 | Keyword search + vector rerank; keyword-only without `arc-llm` | todo |
| M3-07 | Library reader (no LLM) | M | R | U | M3-05, M3-06 | Search, open, follow links, bookmarks | todo |
| M3-08 | Image viewer | S | R | U | M3-05 | Article images show on the framebuffer | todo |
| M3-09 | Assistant with citations and tools | L | D | U | M3-06 | Cited answers; "not found in library" when retrieval is empty | todo |
| M3-10 | Maps part 1: place-name search | M | R | C | M3-04, M3-07 | Gazetteer pack and search | todo |
| M3-11 | M3 done-test + low-tier response time | M | R | I | M3-08, M3-09, M3-10 | Three cited answers from the 32 GB set; library works with `arc-llm` stopped | todo |

### M4 — Real hardware (specs from M4-00)

M4-00 also moves network receive processing out of the hard interrupt into a kernel thread the IRQ wakes (ADR-0019),
and places the kernel log file under `/var/log`, moved here from M1 because real hardware has no disk
driver before M4. Portable mode (ADR-0018) makes M4-04 essential: pulling the key must give I/O errors, never a
crash. M4-01 must switch the USB ports of older Intel chipsets (7- and 8-series) from EHCI to xHCI, as Linux's
xHCI quirk does, or a USB key on those machines stays invisible.

| ID | Block | Size | Tier | Lane | Needs | Done when | Status |
|---|---|---|---|---|---|---|---|
| M4-00 | Plan M4 | S | D | P | M3-11 | M4 block specs written | todo |
| M4-01 | xHCI controller | L | D | K | M4-00 | Ports reset, devices addressed, control transfers work | todo |
| M4-02 | USB core: enumeration and transfers | M | D | K | M4-01 | Descriptors read, config set, bulk and interrupt transfers | todo |
| M4-03 | USB HID keyboard + mouse | M | R | K | M4-02 | USB input arrives on `/dev/input` | todo |
| M4-04 | USB mass storage (BOT) | M | R | K | M4-02 | USB stick is a block device | todo |
| M4-05 | NVMe | M | D | K | M4-00 | Laptop's NVMe readable and writable | todo |
| M4-06 | AHCI | M | R | K | M4-00 | SATA disks readable and writable (QEMU; no SATA test machine now) | todo |
| M4-07 | PS/2 mouse + keyboard on `/dev/input` | S | R | K | M4-00 | PS/2 devices report `input_event`s | todo |
| M4-08 | Intel e1000e (82574 in QEMU) + I219 | M | R | K | M4-00 | Packets flow in QEMU; I219 paths ready for hardware | todo |
| M4-09 | DHCP + DNS | M | R | U | M4-08 | Address from DHCP; names resolve | todo |
| M4-10 | Realtek RTL8111/8168 | M | R | K | M4-00 | Driver follows the datasheet, with Linux's `r8169` read as reference only (GPL, no code copied); hardware test when a Realtek machine is available | todo |
| M4-11 | uACPI: power-off, reboot, buttons, lid | L | D | K | M4-00 | Power button and lid events; clean power-off | todo |
| M4-12 | Battery, profiles, backlight, APST, `power` | M | R | K | M4-05, M4-11 | Battery %, critical shutdown, profiles change watts | todo |
| M4-13 | Maps part 2: map viewer | M | R | U | M4-03 | Pre-rendered tiles; pan and zoom by keyboard or mouse | todo |
| M4-14 | Hardware test campaign | M | R | H | M4-04, M4-06, M4-07, M4-09, M4-10, M4-12, M4-13 | M3's test passes on both machines with and without a cable; watts per profile | todo |

### M5 — Packages and installer (specs from M5-00)

M5 follows ADR-0018: the key builder writes the portable layout straight onto the stick, sized to fill it; the
installer copies the same layout to an internal disk only where a machine allows it; `e2fsck` runs at boot after
an unclean shutdown.

| ID | Block | Size | Tier | Lane | Needs | Done when | Status |
|---|---|---|---|---|---|---|---|
| M5-00 | Plan M5 | S | D | P | M4-14 | M5 block specs written | todo |
| M5-01 | Port e2fsprogs (`mke2fs`, `e2fsck`) | M | R | U | M5-00 | Both run on arc_os against real partitions | todo |
| M5-02 | `arcpkg` tool | M | R | U | M5-00 | install, remove, list, verify, switch role; offline | todo |
| M5-03 | Installer | L | D | U | M5-01, M5-02 | Disk detected, GPT written, system copied, tier and models chosen | todo |
| M5-04 | USB key image builder (host) | M | R | I | M5-03 | One image with installer, packages, packs, models, sources | todo |
| M5-05 | GGUF wrapper + recovery | M | R | U | M5-02 | Any GGUF becomes an `.arcpkg`; `arcpkg verify` repairs from the key | todo |
| M5-06 | Blank-machine test | M | R | H | M5-04, M5-05 | A blank machine becomes a working system from one USB key, offline; the key runs on the USB-only machine without touching its disk | todo |

## 5. Order, critical path and parallel lanes

**Default order:** one block at a time, top to bottom in §4. A block starts only when everything in its Needs is
`done`.

**Critical path** (the longest dependency chain: 35 blocks, 40 session units):

M0-01 → M0-03 → M0-04 → M0-05 → M0-06 → M1-03 → M1-05 → M1-07 → M1-12 → M1-13 → M1-14 → M1-15 → M1-22 →
M2-00 → M2-01 → M2-03 → M2-04 → M2-05 → M2-06 → M3-00 → M3-02 → M3-06 → M3-09 → M3-11 → M4-00 → M4-01 →
M4-02 → M4-03 → M4-13 → M4-14 → M5-00 → M5-01 → M5-03 → M5-04 → M5-06

Inside M1 the chain is per-CPU entry → SMP scheduler → VMAs → page cache → ext2 read → reclaim → ext2 write →
done-test (12 of M1's 28 units). The other M1 blocks hang off that chain and slot in whenever their Needs are met.

**Parallel lanes (optional).** Two sessions at once only when they don't touch the same files:
- M0-07 (schemas) at any time.
- In M1, after M1-05: time (M1-06 → M1-21), drivers and storage (M1-09 → M1-10), the VFS (M1-11, which only needs
  M0-06) and memory (M1-07 → M1-08) are separate areas.
- M4's kernel drivers only need M1. Running M4-00 right after M1-22 would let them go ahead of M2 and M3 if the
  hardware becomes the bottleneck (M4-03 still waits for M3-05).
- The content lane (M3-01, M3-03, M3-04) runs on a Linux host and can start early once M3-00 is written.

Parallel sessions shorten the calendar; they don't save compute.

## 6. Michael's tasks and decisions

| When | What | Default if no answer |
|---|---|---|
| M0-01 | Approve the cloud environment card (setup script) | — (needed to continue) |
| M0-03 | Archive location for outside sources | Release assets on arc_os; private material in a private repo |
| Before M1-04 | In the old work computer's firmware setup, check that Secure Boot can be turned off and booting from USB is allowed (a BIOS password left by the former employer blocks both). No specs to send: the M1-04 boot report records them | Assume it can. If it can't, you decide whether a reference-class laptop (knowledge-os.md §4) replaces it |
| M1-04 | Boot both machines from the USB stick; photograph the boot report and self-test | — |
| M1-22 | Boot both machines to the shell; read idle watts on the laptop | — |
| Before M2-06 | Allow a Remote Control session on the laptop (QEMU with KVM, model downloads) | — |
| Before M3-03 | Where big packs are built: the laptop, or allow the content hosts in the cloud environment | The laptop |
| M3-01 | Approve the content list and licences | — |
| M4-14 | Hardware test campaign on both machines | — |
| M5-06 | Blank-machine install test | — |

## 7. Rules every kernel block follows

- **User memory** is touched only through `copy_from_user`, `copy_to_user`, `get_user`, `put_user` and
  `strncpy_from_user` (exception-table fixups from M1-07). SMAP enforces it from M1-19.
- **Per-CPU state** goes through GS (`this_cpu()`); no new globals for "the current" anything.
- **No periodic timers.** A timer is armed only for a real deadline (ADR-0017).
- **Drivers** use the bus, IRQ and DMA helpers from M1-09.
- **Efficiency** (ADR-0019): no scan of a global list or array in allocate, free, fault, schedule, lookup or
  interrupt paths; any lock an interrupt handler takes is IRQ-safe; no allocation in hard-IRQ context once M1-09
  lands; hot structures carry a `_Static_assert` on their size; hot paths log at debug level only. Each D block's
  **Cost.** section is checked in review, and new metrics go in the budgets file (M0-08).
- **Code hygiene** (ADR-0020): search `docs/kernel/helpers.md` and the tree before writing a helper; a second copy
  of a kernel helper is promoted to the shared header in the same PR. Code outside `kernel/arch/` includes
  `arch/hal.h` or arch-neutral headers, never `arch/x86_64/` directly; a block that rewrites such a call site moves
  it behind the HAL and lowers the limit. New public functions get a doc comment; a new or rewritten `.c` file opens
  with what it owns, its invariants and its locks. Cleanup only in files the block already edits, in a separate
  `tidy:` commit. The hygiene counts (M0-09) never rise.
- **ABI structs** live in `kernel/include/uapi/`, carry `_Static_assert`s, and `tests/test_abi_layout.c` checks
  them against the host's Linux headers.
- **Lock order**, outermost first. Debug builds check it from M1-05; M1-05 copies this into
  `docs/kernel/locking.md`. It follows the order Linux documents for the same structures.
  1. inode lock (sleeping; write, truncate, rename, directory changes)
  2. mm lock (sleeping read-write; mmap, munmap, mprotect, page faults)
  3. page lock (sleeping, per page)
  4. mapping-tree lock (sleeping; the list of VMAs mapping a file, used by reclaim and truncate)
  5. page-table lock (spinlock, per mm)
  6. page-cache index lock (spinlock, per address space)
  7. thread-group and signal lock (spinlock)
  8. wait-queue locks (spinlock)
  9. run-queue locks (spinlock, interrupts off; two at once only in CPU-index order)
  10. leaf spinlocks: buddy allocator, timer queues, log buffer

  Never sleep while holding a spinlock. The fault path never takes an inode lock. Never take a page fault while
  holding a page lock: prefault the user buffer, copy with faults disabled, and retry on a short copy.

## 8. Constraints found while planning

| # | Finding | Handled in |
|---|---|---|
| 1 | Cloud sessions have no KVM, so QEMU runs in TCG (roughly 10–50× slower) | M0-02: targeted tests in sessions, full suites in CI; performance only on the laptop (M2-06) |
| 2 | Cloud sessions can't reach ftp.gnu.org, musl.libc.org, download.kiwix.org or huggingface.co | M0-01/M0-03: GitHub Actions fetches and archives; big content and models on the laptop |
| 3 | `scratchpad*.md` is gitignored and cloud containers are thrown away | §3 rule 7: handoff in the block's Notes, committed |
| 4 | Cross-compiler path is hard-coded to `$HOME/opt/cross` | M0-01: `ARC_CROSS_PREFIX` |
| 5 | Kernel built without `-mgeneral-regs-only`: GCC may use SSE in kernel code, and user SSE state is never saved | M0-06 (stock musl needs SSE before `main`: `-mgeneral-regs-only`, user SSE, FXSAVE per thread; Michael's OK 2026-11-08); M1-01 moves to XSAVE/AVX |
| 6 | `MAX_CPUS` is 16; the laptop has exactly 16 CPUs and desktops have more | M1-03: 256 |
| 7 | The Core Ultra laptop's firmware may hand over in x2APIC mode | M1-03 |
| 8 | No log file is possible before storage drivers exist (M4) | M1-04: framebuffer log, panic screen and boot report; log file on disk after M4 |
| 9 | A USB keyboard on the low-tier machine won't work before xHCI unless firmware legacy USB emulation is on | M1-04: report needs no input; M4-03 |
| 10 | VFS reads and writes take 32-bit offsets (4 GiB cap) | M1-11 |
| 11 | A process holds a single thread | M1-16 |
| 12 | No poll/select infrastructure | M1-18 |
| 13 | `exec` reads argv through raw user pointers | M0-06, M1-07; SMAP catches the rest (M1-19) |
| 14 | Old slash commands sent sessions to `overview.md`, and `/os/add-syscall` assigned arc_os's own numbers | Fixed in this PR |
| 15 | One data partition must serve `/apps /models /packs /home /var` (ADR-0009) | M1-11: subtree mounts; M1-13 mounts them |
| 16 | The installer needs `mke2fs`/`e2fsck` on arc_os and runs from the USB key | M5-01; M5-03 after M4-04. The ESP is written from a prebuilt FAT32 image |
| 17 | QEMU's e1000e is the 82574, not the I219 | M4-08 in QEMU; I219 checked on hardware in M4-14 |
| 18 | No USB hub support is planned | Fine if both machines' ports are root ports; checked in M4 |
| 19 | M0/M1 musl tests can use Ubuntu's `musl-gcc` (C only); C++ waits for the musl toolchain | M2-01; M0-04 proves the C++ build early in an Alpine container |
| 20 | ADR-0001's syscall migration table was written from memory | M0-04 verifies it against the archived `syscall_64.tbl` |
| 21 | The low-tier machine can't keep installs; arc_os must run entirely from the USB key | ADR-0018: M1-02 (`arc.mem`), M1-04 (RAM mode), M1-10/M1-13 (own partition types, mount from the boot disk), M1-15 (no atime), M4-04, M5 |
| 22 | Older Intel chipsets (7- and 8-series) route USB ports to EHCI until the OS switches them | M4-01 |
| 23 | No test machine writes to a SATA disk | AHCI writes tested in QEMU only; if the old work computer's disk is SATA, M4-14 covers AHCI reads (read-only, ADR-0018) |
| 24 | A machine with Secure Boot locked on can't boot unsigned Limine | Off for now; shim + own enrolled key is the known path (ADR-0018), a separate decision |
| 25 | Neither test machine nor QEMU has a Realtek RTL8111/8168 | M4-10 ships untested on hardware; M4-14 tests it if a Realtek machine is available by then |
| 26 | The kernel heap had no lock, but the virtio-net IRQ path allocates from it | Fixed with ADR-0019 (IRQ-safe heap lock + host test) |
| 27 | No object allocator was planned; kmalloc is a first-fit list | M1-02b (ADR-0019) |
| 28 | CI can't time code (TCG), and nothing checked efficiency before M2-06 | M0-08 budgets on counts and sizes; `arc-bench` at M1-04, M1-22, M4-14, M5-06 (ADR-0019) |
| 29 | Duplicate helpers, 44 x86 includes in 8 subsystems outside `kernel/arch/`, and no check on either | M0-09 freezes the counts; M1-03, M1-05, M1-06, M1-07, M1-09 move the includes they touch behind `arch/hal.h` (ADR-0020) |

## 9. Later: compatibility track (not counted)

After M5, for machines Michael chooses to support (ADR-0018). Each item becomes blocks only with his OK:
USB hubs and docks; more network chips and USB Ethernet adapters; I2C-HID
touchpads; AMD power management (CPPC instead of Intel HWP); chipset and firmware quirks found on new machines;
Secure Boot through a signed shim and our own key.

## 10. Change log

- **2026-11-07** — First version: 70 blocks over M0–M5, full specs for M0 and M1, session protocol, lock order.
- **2026-11-07** — Portable mode (ADR-0018) at Michael's request: fitted into M1-02, M1-04, M1-10, M1-13, M1-15,
  M4-01, M4-04 and M5 with no new blocks; low-tier machine is now Michael's old work computer (USB-only);
  compatibility track listed for later.
- **2026-11-07** — Low tier defined as a class (Michael): a standard used business laptop, about $150–300 CAD;
  the 4 GB floor stays. The old work computer stands in for it; its parts come from the M1-04 boot report, so the
  "send details" task became a firmware check. M1-04 reports the machine model (SMBIOS); M4-13 pans by keyboard
  too; M3-00 plans for 1366×768 screens; USB Ethernet adapters and I2C touchpads listed in the compatibility track.
- **2026-11-08** — Michael kept the Realtek driver (M4-10): the OS ships Intel and Realtek Ethernet drivers; more
  chips later if wanted. Network stays optional; models and packs never need it.
- **2026-11-08** — Efficiency review (ADR-0019, Michael approved): new blocks M0-08 (budget checks) and M1-02b
  (slab allocator, before M1-05); 32-byte page descriptors (M1-02); order-agnostic page-cache lookups (M1-12); log
  levels (M1-04); network rx out of hard IRQ (M4-00); M2-06 compares mmap with 2 MiB anonymous loading; **Cost.**
  sections on M1 D blocks; efficiency rules in §7; `arc-bench` runs at the hardware checkpoints.
- **2026-11-08** — Maintainability review (ADR-0020, Michael approved): new block M0-09 (hygiene ratchets in CI);
  §3 rule 13 (block review before every PR); code-hygiene rules in §7; HAL include moves added to M1-03, M1-05,
  M1-06, M1-07 and M1-09; hygiene pass in M1-22.
- **2026-11-08** — M0-08's leak check found that every exited process leaks ~54 pages and ~40 KB of kernel
  heap (`wait4` frees nothing). Michael approved a new block, M0-10 (free reaped processes); M0-08 freezes the
  leak budgets at today's level until then. M0 order from here: M0-08, M0-09, M0-10, then M0-07.
