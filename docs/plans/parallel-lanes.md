# Parallel lanes — building more at once without losing accuracy

> **Status: APPROVED (Michael, 2026-11-12, all six decisions in §1).** Applied in
> [`work-blocks.md`](work-blocks.md) (§3 rules 10 and 15, §5, §6, §7, new rows) and recorded in
> [ADR-0021](../decisions/0021-parallel-lanes-stacking-review.md). This file keeps the reasoning and the numbers;
> where the two differ, work-blocks.md wins. One change while applying: the QR block is split into M1-04b
> (encoder, new files only, runs beside M1-05) and M1-04c (drawing, after M1-05), so the encoder can start now.

Read at `origin/master` `ba05a67` (PR #31 merged). Numbers come from a small scheduling script that reads the §4
tables of `work-blocks.md` (kept in project files: `arc_os/plans/lane-schedule-sim.py`). Units are the plan's session units
(S 0.5, M 1, L 2); "sessions" adds the plan's debugging allowance (D × 1.5, R × 1.2). They are estimates.

## 1. Decisions for Michael

| # | Decision (all approved 2026-11-12) | Recommendation | Default if no answer |
|---|---|---|---|
| D1 | Two kernel lanes after M1-05 merges (§3) | **Yes.** It is the one change that moves the finish date a lot: the remaining work takes about 30 units instead of 44.5 | Stay at one kernel lane |
| D2 | Start the next block in a lane when the previous PR is CI-green and has a clean independent review, branched on top of it (§4) | **Yes**, within a lane, at most one unmerged PR underneath | Wait for each merge |
| D3 | Independent review of every D-tier kernel PR by a thread that didn't write it, before you review (§5) | **Yes.** Two lanes mean twice the PRs, so this protects your review time as well as accuracy | No independent review |
| D4 | Hardware loop: CI publishes the bring-up image; a laptop "smoke boot" after each hardware-sensitive block, at most one such block unverified at a time (§6) | **Yes** | Only the four checkpoints (M1-04, M1-22, M4-14, M5-06) |
| D5 | QR codes for exact text off the screen: new block M1-04b (§6.4). New scope | **Yes.** Every later hardware check and every panic on real hardware gets exact text instead of photo transcription | Photos only |
| D6 | Two dependency trims (§7): M3-05's spec moves to M4-00 so it can run right after M1-22; M5-01 and the installer core become Linux-first (new planning block M5-00a) | **Yes to both**, about 3 units off the end | Plan as is |

Not proposed, on purpose: dropping or thinning host or QEMU tests, the verify-before-PR rule, merging blocks into
bigger PRs, or a third kernel lane (§2 shows it buys almost nothing).

## 2. Where the time goes

From M1-05 (in progress) to M5-06, 55 blocks and 61.5 units remain. The longest chain of Needs is 24.5 units, but
that assumes unlimited parallel work. The real limit is the kernel lane: M1 still has 24 units of kernel blocks,
and after M1-22 the M4 drivers add about 13 more, all in the same single lane. With one kernel lane, the M4
drivers, not the knowledge layer, end up deciding when M4-14 can run.

| Kernel lanes | M1-22 done | M3-11 done | M4-14 done | All done | Sessions (with allowance) |
|---|---|---|---|---|---|
| 1 (today) | 24 | 32.5 | 39 | **44.5** | 62 |
| 2 | 14 | 24 | 25 | **30.5** | 42 |
| 2 + trims (§7) | 14 | 23.5 | 24.5 | **27.5** | 37.5 |
| 3 | 13 | 22.5 | 23.5 | 29 | 41 |

Units from now. The simulation keeps the user-space, content and Linux-first lanes at one block at a time each,
counts each Michael hardware step as one unit, never runs anything beside M1-05, and never runs two blocks at once
that edit the same files (§3.3). Total compute barely moves (61.5 → 62.5 units with the trims): parallel lanes
shorten the calendar, they don't save sessions.

The model leaves out the biggest real gap: **idle time between blocks.** Merges land in evening batches
(`git log`: PRs #21–#31 all merged between 18:35 and 22:10 Toronto time), and a kernel block only starts after its
parent merges, so a lane can sit idle most of a day per block. Stacking (§4) closes that gap; lanes don't.

## 3. Kernel lanes

### 3.1 Why nothing kernel runs beside M1-05

M1-05 is the hub: every remaining M1 block needs it except M1-11 (VFS), and M1-11 can't safely go first.
Today syscalls run with interrupts off, and that is the only thing protecting the process list, PID counter, VFS,
ramfs and fd tables. M1-05 is deciding how that changes (a big kernel lock is on a decision card in its thread).
M1-11 rewrites exactly the VFS and fd tables, so starting it now would build on locking that isn't decided yet.
That's the kind of rewrite risk the project rules say to flag before building.

**Safe to start now, beside M1-05:** M3-01 (content selection). It doesn't touch the kernel. The private-repo
question has a default (project files), and the ZIM-reader choice only blocks M3-03.

### 3.2 The split after M1-05 merges

The lanes follow the lock order in work-blocks §7. Locks 1–6 (inode, mm, page, mapping tree, page table,
page-cache index) belong to lane A; locks 7–10 (thread group and signal, wait queues, run queues, leaf locks)
belong to lane B. Each lane designs and documents only its own locks, so two threads never invent overlapping
rules for the same lock.

**Lane A — memory and files** (the critical path):

| Order | Block | Units | Runs while lane B does |
|---|---|---|---|
| 1 | M1-07 VMAs, demand paging | 2 | M1-09, M1-10 |
| 2 | M1-11 VFS rework | 2 | M1-06, M1-21 |
| 3 | M1-12 page cache + file mmap | 2 | M1-16 |
| 4 | M1-13 ext2 read | 1 | M1-17 |
| 5 | M1-14 reclaim + writeback | 1 | M1-18 |
| 6 | M1-15 ext2 write | 2 | M1-20, M1-08 |
| 7 | M1-19 hardening | 1 | (idle) |

**Lane B — CPU, time, devices, threads:**

| Order | Block | Units | Why here |
|---|---|---|---|
| 1 | M1-09 driver model, MSI-X | 1 | Needs only M1-05 |
| 2 | M1-10 async block + GPT | 1 | M1-12 needs it. Runs beside M1-11 (both edit `fat32.c`; see §3.3) |
| 3 | M1-06 clocks, tickless | 1 | After M1-09 (both edit `lapic.c`); after the M1-05 smoke boot (§6.2) |
| 4 | M1-21 power base | 1 | Needs M1-06 |
| 5 | M1-16 threads I | 2 | Needs M1-07 from lane A |
| 6 | M1-17 threads II | 1 | |
| 7 | M1-18 AF_UNIX + poll | 1 | Needs M1-11 from lane A |
| 8 | M1-20 syscall gap fill | 1 | |
| 9 | M1-08 COW fork + huge pages | 1 | Edits `vmm.c`, so it waits until lane A is in ext2 code (M1-15), not in `vmm.c` |

M1-22 runs once both lanes finish (about 12 units after M1-05 merges, against 22 in one lane). With D5 approved,
the QR work is split: M1-04b (encoder, 1 unit) runs now beside M1-05, because its files are all new, and M1-04c
(drawing, 0.5 units) goes into lane B after M1-10, before M1-06. Lane B has about 1 unit of slack, so M1-22 doesn't
move, and every smoke boot from M1-06 on gets exact text.

**After M1-22**, the same two lanes carry on. Lane A takes storage and USB: M4-00, M4-01 xHCI, M4-02 USB core,
M4-04 USB storage, M4-05 NVMe, M4-06 AHCI. Lane B takes M3-05 (console, fb, input; critical path), M4-08 e1000e,
M4-10 Realtek, M4-11 uACPI, M4-12 battery, then M4-07 and M4-03 once M3-05 is in. User space runs M2-00 → M2-04b →
M2-05 at the same time, and the content lane runs M3-01 → M3-03 → M3-04.

### 3.3 Where the lanes touch (the accuracy risk)

The file overlaps were read from each block spec's "Read first" and "Build" sections, then checked against the
order above.

| Pair (lane) | Shared | Rule |
|---|---|---|
| M1-10 (B) / M1-11 (A) | `fat32.c` | Changed 2026-11-13 (Michael): M1-11 went first and kept FAT32's three `dev->read` calls; whichever merges second swaps or rebases them |
| M1-06 (B) / M1-09 (B) | `lapic.c`, `hal.h` | Same lane, in order |
| M1-06, M1-16, M1-21 (B) / M1-05 | `sched.c` | Same lane, in order |
| M1-07, M1-08, M1-12, M1-19 | `vmm.c` | Never two at once; M1-08 waits as above |
| M1-11 (A) / M1-16 (B) | `fd.c` (shared fd tables for threads) | M1-16 starts after M1-11 merges |
| M1-12 (A) / M1-16 (B) | `sys_execve` (exec maps from the page cache; exec in a thread group kills the other threads) | Run at the same time; M1-16 changes only thread teardown in exec, and whichever merges second rebases. The independent review checks this pair |
| `hal.h`, `helpers.md`, `budgets.md`, `locking.md`, `work-blocks.md` | Shared lists | Add-only edits; the second PR rebases. Conflicts here are textual, not design |

**Cross-lane interfaces.** These are where one lane consumes what the other produced, and where a parallel
mistake would cost the most:
- M1-10 → M1-12: block I/O completion wakes page-cache waiters. The completion side may take only wait-queue and
  leaf locks (§7 locks 8 and 10).
- M1-07 → M1-16: threads share an mm; M1-11 → M1-16: threads share an fd table (both refcounted).
- M1-11 → M1-18: poll goes through the VFS file operations.
- M1-06 → M1-17 and M1-18: timed waits and poll timeouts use M1-06's timer API.

**Rule:** a block that consumes a cross-lane interface starts only after the producing block has **merged**. No
stacking across lanes, so a consumer never builds on an interface that review might still change.

**The big kernel lock, if M1-05 adds one:** `docs/kernel/locking.md` gets a "BKL coverage" table, one row per
subsystem still under it. Only the lane that owns a subsystem edits its row (lane A: VFS, ramfs, fd tables, mm;
lane B: process list, PID counter, signals, sockets via M1-18). The last block to take a subsystem off removes the
lock itself. If Michael decides against a BKL, the split stays the same.

### 3.4 What two lanes cost

- **Michael's review load doubles.** D3 (independent review) is the counterweight. A WIP limit keeps it bounded:
  at most two unmerged PRs per lane and four kernel PRs in total. When the limit is hit, the lane waits.
- **Rebase work** on the shared lists above: minutes per PR.
- **More parallel threads:** two kernel threads plus the content lane. Each one still runs one block in one
  session, with the same tests.

## 4. Stacking: start the next block on a CI-green parent

**How it works.** When a lane's PR is CI-green and its independent review (§5) has no open findings, the lane's
next block starts on a new branch made from the parent's branch. Its PR targets the parent's branch, so the diff
shows only its own changes. When Michael merges the parent, the child PR is retargeted to `master`. Michael still
merges every PR, in order within the lane, and one block stays one PR.

**Why it is cheap here.** PRs land as merge commits (not squashes), so after the parent merges, the child's copy
of the parent's commits is identical to `master`'s. The child just merges `origin/master`, with no history
rewrite. This only holds if **review fixes on a parent go on as new commits and the parent is never force-pushed**
once a child exists.

**Rebase cost.**
- If review changes only the parent's internals: a merge of `origin/master` and a test rerun, a few minutes.
- If review changes an interface the child uses: the child redoes that part. Estimate up to half a session. That's
  why stacking waits for the independent review: most interface findings are caught before the child starts.

**When not to stack:**
1. A decision card on the parent is still open. M1-05's lock card is one: nothing stacks on M1-05 until Michael
   answers it.
2. The child would build on a cross-lane interface (§3.3). Those wait for the merge.
3. There's already one unmerged PR under the parent. Depth is limited to 1.
4. The parent is hardware-sensitive and its smoke boot (§6.2) hasn't happened, and the child builds on the same
   hardware behaviour. Example: M1-21 on M1-06's timer code.
5. Planning blocks (M2-00, M3-00, M4-00, M5-00). They depend on measurements and merged results, not branches.

The coordinator starts the stacked block when conditions 1–5 are clear. With stacking on, work-blocks §5's "a
block starts only when everything in its Needs is `done`" becomes "`done`, or `review` with a clean independent
review, same lane, depth 1".

## 5. Independent review of kernel PRs

**What:** after the author thread opens a D-tier kernel PR (R-tier when it touches locking or IRQ code), the
coordinator starts a fresh review thread. It gets the PR number, the block spec and the ADRs the spec cites. It
doesn't see the author's session or reasoning, so it isn't anchored on the author's view.

**What it checks.** It doesn't repeat `block-review`; it verifies it and goes deeper:
1. **Re-runs the claims.** It builds, runs the block's targeted QEMU test and `tools/hygiene.py`, and checks the
   PR's block-review table against the diff. A "PASS" without evidence counts as a finding.
2. **Spec and ADRs:** every Build step and Done-when item is in the diff or explained in Notes. Nothing beyond scope.
3. **Concurrency:** lock order against §7 and `locking.md`; IRQ-safe locks for anything an interrupt touches; no
   sleep under a spinlock; every early return releases what it took; SMP races on shared fields. For each new
   lock it writes one line: who takes it, from which context, and what it nests under.
4. **Latent bugs:** error paths, leaks (the `pages_leaked` and `heap_leaked_bytes` budgets), integer widths
   (64-bit offsets, LBAs), user pointers only through the copy helpers, TCG-vs-KVM timing assumptions (waits
   bounded by TSC, not loop counts).
5. **The cross-lane pairs** in §3.3 that this PR touches.

**Output:** one PR comment, with findings ranked by severity as CONFIRMED (reproduced or provable from the code)
or PLAUSIBLE, each with `file:line` and a failure scenario. "No issues found" is a valid result. The author
thread fixes CONFIRMED findings as new commits. Michael then reviews a PR that has already had a hostile read.

**Cost:** about a third of a session per PR. 17 D-tier kernel blocks remain after M1-05 (M1-06 … M1-19, M3-05,
M4-01, M4-02, M4-05, M4-11), so about 6 sessions in total, plus a few R blocks that touch locking. That's
cheap next to one kernel bug found three blocks later.

**How it fits ADR-0020:** `block-review` stays the author's own check (rule 13). The independent review becomes
§3 rule 15 and adds its comment link to the PR body. It uses `/validate/review` or `code-review` at medium
effort as its engine where those fit, and never edits the branch itself.

## 6. The hardware loop

### 6.1 Today

What already exists: the bring-up stick (`cmake --build build --target iso-bringup`, guide in
`docs/hardware/bringup.md`), results template `docs/perf/README.md`, and the low-tier row in knowledge-os.md §4.
CI uploads `arc_os.iso`, the normal image, as a 14-day workflow artifact, but **not** the bring-up image. A
workflow artifact also needs a GitHub login and comes zipped. No hardware boot has happened yet, so every kernel
block so far is verified in QEMU only.

**Disk safety, checked in code at `ba05a67`:** the only disk driver is virtio-blk. It binds only to PCI
`1AF4:1001`, a device that exists only in virtual machines (`kernel/drivers/virtio_blk.h:7-8`,
`virtio_blk.c:30`), and FAT32 is mounted only on that device (`kernel/boot/kmain.c`, `virtio_blk_setup`). There's
no NVMe, AHCI, IDE or USB-storage code, and nothing calls UEFI runtime services. On real hardware nothing in the
kernel can write to a disk. This changes at M4-04, M4-05 and M4-06, and ADR-0018 governs from there.

### 6.2 Proposed loop (D4)

1. **CI publishes the bring-up image.** On every push to `master`, CI builds `iso-bringup` and replaces the asset
   of one pre-release named `bringup-latest` with `arc_os-bringup.iso` and its SHA-256. A public release asset needs
   no login and no unzip and is always the newest. Only that job gets `contents: write`. Size S, lane I.
   - The boot report's first line gains the git commit (a `-D` define at build time), so every photo names its
     build.
2. **Hardware-sensitive blocks** get a laptop smoke boot. This is about 15 minutes: write the stick, boot the
   default entry, photograph the report and self-test.

   | Block | Why QEMU can't vouch for it | Machines |
   |---|---|---|
   | M1-05 SMP scheduler | 16 hybrid P/E CPUs running real threads, real IPI latency | Laptop |
   | M1-06 clocks, tickless | TSC source, PIT present or gated off, TSC-deadline, whether the APIC timer keeps running in deep C-states (ARAT) | Both |
   | M1-09 MSI-X | Real devices' MSI-X tables (read and listed, not driven) | Laptop |
   | M1-19 hardening | SMEP/SMAP/NX on real CPUs and firmware page tables | Laptop |
   | M1-21 power base | MWAIT C-states, HWP/EPP and RAPL. QEMU emulates none of them | Both |

   **Gate:** at most one hardware-sensitive block merged without its smoke boot. The next hardware-sensitive PR
   waits for it, and other blocks keep moving. The four checkpoint boots (M1-22, M4-14, M5-06 and the overdue
   M1-04) stay as they are.
3. **Each hardware-sensitive block adds its own report or self-test line**, so the smoke boot proves the block
   (for example M1-06: "tsc-deadline fired n/n, idle CPU timer IRQs 0"). The M1-04 spec already does this for the
   benchmark.
4. **Recording:** Michael posts photos in the thread or PR. A thread transcribes them into
   `docs/perf/<date>-<machine>.md` (bench), `docs/hardware/<machine>.md` (report: new file per machine) and the §4
   Low row. Every value is marked "read from photo", and an unclear digit is left blank, never guessed. With QR codes
   (D5) the text is exact and the marking goes away.

**Where a VM fits:** QEMU in CI (KVM) stays the gate for every PR, because it is repeatable and checked over
serial. Real hardware is the only place firmware tables, timers, CPU topology and power features are real. The
loop above makes it regular without making it per-PR.

**Missing from tomorrow's boot:** today's Features line prints AVX2, FMA, BMI2, XSAVE, MWAIT, SMEP, SMAP, NX,
1G-PAGES and RDRAND. It doesn't print TSC-deadline, ARAT, HWP or RAPL, which M1-06 and M1-21 need. The first block
after M1-05 that touches `kernel/diag/report.c` adds them: M1-04c (item 3). Tomorrow's photos still settle the CPU
floor, the TSC source, PIT behaviour, the memory map, the ACPI tables and the PCI devices.

### 6.3 Getting exact text off a machine with no serial port

Until M4 there's no storage driver for real disks, no USB driver and no real network driver, and the laptops
have no serial port. The screen is the only output. The options:

| Option | Exact text | Available | Cost |
|---|---|---|---|
| Photos, transcribed | No (digits misread, long hex lines) | Now | Transcription time; errors |
| QR codes drawn by arc_os, read by a phone | **Yes** | After M1-04c | M1-04b + M1-04c (§6.4) |
| Log file on the USB stick | Yes | After M4-04 (USB storage) | In plan |
| Network log (netconsole-style) | Yes | After M4-08 (I219), plus new code | Not planned |
| USB serial adapter | Yes | After xHCI + a USB serial driver | Not planned |

QR is the only exact channel before M4, and it keeps working afterwards for panics that happen before storage is up.

### 6.4 D5: blocks M1-04b and M1-04c, QR codes for the report, the log and panics

**What Michael sees:** the bring-up stick gets a fourth menu entry, "boot report as QR codes" (`arc.qr=1`). The
report, self-test and bench pages are each drawn as numbered QR codes, one per screen, each held for 10 s or until
a key is pressed. Michael points his phone camera at each one and copies the text into the thread. On a panic, the
panic screen shows one QR code beside the registers, with the panic reason, registers and backtrace in it.

**Payload (plain text only):**
```
ARC1 <commit> <kind> <page>/<pages> <crc32-of-this-page's-text>
<text lines>
```
- It starts with `ARC1`, so no phone treats it as a link, a Wi-Fi join, a contact or a phone number. Those
  actions need a payload that starts with a scheme such as `http:`, `WIFI:`, `MECARD:` or `tel:`, or is a bare
  web address.
- The encoder accepts printable ASCII and newline only; anything else is written as `\xNN`, and any `://` is
  written as `:\/\/`. A code can never hold a link, even if a log line someday contains one.
- `kind` = `report`, `log` or `panic`. The CRC lets the reading thread prove the pasted text is exactly what the
  kernel drew. The last page also carries a CRC of all pages, so a missing page shows.

**Size per code** (QR byte mode; capacities from the QR standard's table, to be confirmed by the round-trip test):
version 30 at error-correction level M holds 1,370 bytes in 137 × 137 modules. With the quiet zone that is 145
modules: 5 pixels per module on a 768-pixel-high screen (the low-tier class) and 9 on 1350. Phones read 4+ pixel
modules reliably off a screen.
- **Boot report:** 2.3 KB in QEMU (35 lines, 6 PCI devices). A real laptop with about 35 PCI devices and about
  30 ACPI tables comes to roughly 4–5 KB (my estimate): **4 codes**.
- **Log** (`arc.qr=log`): the newest 8 KB of the kernel log, 6 codes. QEMU's whole boot log is 10.7 KB.
- **Panic:** one code (reason, registers, CR2, backtrace: under 1 KB).

**Files and size:**

| File | What | Lines (est.) |
|---|---|---|
| `kernel/diag/qr.c`, `qr.h` | Encoder: byte mode, versions 10–30, level M, Reed–Solomon over GF(256), the 8 masks with the standard penalty score, format and version bits. Static buffers only, no allocation, so the panic path can use it | ~450 |
| `kernel/diag/qr_pages.c` | Split text into pages, header and CRC, scale and center, page timer | ~150 |
| `kernel/drivers/fb_console.c` | One function to draw a 1-bit bitmap scaled, while the console is paused | ~40 |
| `kernel/diag/report.c`, panic screen, `make-iso.sh` | `arc.qr=1`, panic code, fourth menu entry; commit in the report's first line; TSC-DEADLINE, ARAT, HWP and EPP on the Features line (RAPL has no CPUID bit on Intel; M1-21 reports it) | ~70 |
| `tests/test_qr.c` + check script | Host round trip: encode about 50 texts (empty, 1 byte, exact capacity, every version, random bytes), render to PGM, decode with `zbarimg` (Ubuntu `zbar-tools`, host only, added to `dev-setup.sh`) and compare byte for byte. Plus known-answer tests for Reed–Solomon and the format bits | ~250 |
| QEMU suite `qr` | Boot `arc.qr=1`, take a QEMU `screendump`, decode it, compare with the `[REPORT]` lines on serial | ~80 |

That totals about 1,000 lines with tests. As applied, it is two blocks: **M1-04b** (encoder, CRC, text pages and
host round trip; M, R) and **M1-04c** (drawing, panic code, menu entries, QEMU screendump test; S, R). Own code, no outside library: an encoder is small and fully checked by the decoder round trip, and a
kernel library would add an outside component to archive. A crc32 helper doesn't exist yet (`docs/kernel/helpers.md`), so this block adds it to `kernel/lib` and lists it there.

**Conflict with M1-05:** yes, a small one. M1-05 makes `kprintf` and the log ring SMP-safe, and may change who
owns the console, and the QR code reads the log ring and pauses the console. So the drawing half (**M1-04c**) goes
after M1-05 merges, in lane B. The encoder half (**M1-04b**) adds only new files (plus add-only lines in the CMake
lists, `helpers.md` and `dev-setup.sh`), so it runs now, beside M1-05.

**Is a QR code safe to scan?** A QR code is only text. The risk is what the phone does with that text: open a
link, join a Wi-Fi network, call a number. Ours always starts with `ARC1` and can't contain a link, so the phone
shows text and nothing else. Two habits make any code safe to check: use a scanner that shows the text before
doing anything (the iPhone and Android camera apps show a preview), and if a code ever offers to open a website,
join a network or call someone, don't tap it.

## 7. Two dependency trims (D6)

1. **M3-05 (console, `/dev/fb0`, `/dev/input`) is written up by M4-00 and needs only M1-22.** Today it waits for
   M3-00, which waits for the laptop measurements in M2-06. But M3-05 is kernel work, and the only M3 rule it needs
   (the 1366×768 minimum screen) is already in work-blocks.md. It's on the critical path, so it saves 0.5 units
   there and unblocks M4-03 and M4-07 earlier.
2. **A second Linux-first round for M5:** a new planning block M5-00a (S, needs M1-15, when the ext2 features are
   final) writes specs for M5-01 (port `mke2fs`/`e2fsck`) and the installer core of M5-03. Both are built and
   tested on Linux with musl inside the syscall inventory, against disk image files, exactly as lane L did for
   M2. A new S block M5-03b runs the installer on arc_os after M4-14. This takes about 3 units off the M5 tail
   (M5-00 → M5-01 → M5-03 → M5-04 → M5-06) and costs about 1 unit more compute (the planning and bring-up blocks).
   The installer partly depends on M4's disk drivers, but it only sees them through block devices and Linux
   ioctls (ADR-0001), so it doesn't need to wait for them.

Also checked and **not** proposed: making the reader (M3-07), image viewer (M3-08) and map viewer (M4-13)
Linux-first. They need `/dev/fb0` and `/dev/input`, which cloud sessions don't have, and faking them would test
the fake. M3-00 can revisit this.

## 8. Applied (2026-11-12)

PR #32 applies all six:
- work-blocks.md:
  - §5: the lanes and the stacking rule.
  - §3: rule 15 (independent review) and rule 10 extended (smoke boots).
  - §6: a smoke-boot row.
  - §7: the lane ownership of lock levels. `docs/kernel/locking.md` doesn't exist yet; M1-05 creates it from §7.
  - New rows for M1-04b, M1-04c, M5-00a and M5-03b, with totals recomputed by script (83 blocks, 85.5 units).
- Specs for M1-04b and M1-04c in `blocks/`.
- `/os/implement-block` and `/os/block-status` know about stacked starts.
- ADR-0021 set to Accepted.

The CI image publishing (D4 item 1) is a separate small infra PR. Kernel lanes start when M1-05 merges, and M1-04b
can start now.
