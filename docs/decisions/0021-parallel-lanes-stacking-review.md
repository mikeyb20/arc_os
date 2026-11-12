# ADR-0021: Parallel kernel lanes, stacked blocks and independent review

- **Status:** Proposed (draft for Michael, 2026-11-12). Not binding. Each part becomes Accepted only with
  Michael's explicit OK, and only the approved parts are kept.
- **Date:** 2026-11-12
- **Amends:** `docs/plans/work-blocks.md` §3 (rules 10 and 13; new rule 15) and §5 (order and lanes). Supersedes
  no ADR. Details and numbers: [`docs/plans/parallel-lanes.md`](../plans/parallel-lanes.md).

## Context
- After M1-05, 55 blocks and 61.5 session units remain. One kernel lane, running strictly in order, finishes in
  about 44.5 units. The kernel lane is the limit: 24 units of M1 work, then about 13 of M4 drivers.
- Many M1 blocks don't depend on each other once M1-05 lands (time, drivers, VM, VFS, threads). The lock order
  in work-blocks §7 splits cleanly into memory-and-files locks (1–6) and CPU/thread/leaf locks (7–10).
- PRs merge in evening batches, and a kernel block starts only after its parent merges, so lanes sit idle
  between blocks.
- Kernel correctness comes first (Michael, 2026-11-08). Every block still runs host and QEMU tests and the
  block review, and one block is still one PR. Nothing here changes those rules.
- Real hardware has run nothing yet. The laptops have no serial port, and the bring-up image isn't published by
  CI.

## Decision (proposed)
1. **Two kernel lanes after M1-05 merges.**
   - Lane A, memory and files: M1-07, M1-11, M1-12, M1-13, M1-14, M1-15, M1-19.
   - Lane B, CPU, time, devices and threads: M1-09, M1-10, M1-06, M1-21, M1-16, M1-17, M1-18, M1-20, M1-08.
   - Lane A owns §7 locks 1–6 and lane B owns 7–10. Only the owning lane changes a lock's rules, or its row in
     the big-kernel-lock coverage table if M1-05 adds one.
   - Two blocks that edit the same file never run at the same time, except the add-only shared lists and the
     M1-12/M1-16 exec pair listed in parallel-lanes.md §3.3.
   - A block that consumes the other lane's interface starts only after the producing block merges.
   - After M1-22: lane A takes storage and USB in M4; lane B takes M3-05 and the other M4 drivers.
   - WIP limit: two unmerged PRs per lane, four kernel PRs in total.
2. **Stacking within a lane.** The next block may start on the branch of a CI-green parent whose independent
   review has no open findings.
   - Depth is limited to 1, and never across lanes.
   - Not while a decision card on the parent is open, and not on a planning block.
   - Parents take review fixes as new commits and are never force-pushed once a child exists.
   - Michael still merges every PR, in order within the lane.
3. **Independent review** of every D-tier kernel PR, and of R-tier PRs that touch locking or interrupt code.
   - It is done by a fresh thread that didn't write the code, given only the PR, the block spec and the cited ADRs.
   - It re-runs the block-review evidence and checks lock order, IRQ safety, error paths, leaks and the
     cross-lane pairs.
   - It reports CONFIRMED or PLAUSIBLE findings in one PR comment, and the author fixes them before Michael
     reviews.
   - `block-review` stays the author's own check (ADR-0020).
4. **Hardware loop.**
   - CI publishes the bring-up image as the `bringup-latest` pre-release on every push to `master`, and the report
     names its commit.
   - The hardware-sensitive blocks M1-05, M1-06, M1-09, M1-19 and M1-21 each get a laptop smoke boot (M1-06 and
     M1-21 on both machines).
   - At most one hardware-sensitive block may be merged without its smoke boot.
   - Each such block adds a report or self-test line that proves it on hardware.
5. **(Separate yes/no, new scope) Block M1-04b:** the boot report, log tail and panic screen drawn as plain-text QR
   codes (`ARC1` header, page counter, CRC32, no links possible), with our own encoder and a host round-trip test
   against `zbarimg`.
6. **(Separate yes/no) Dependency trims.**
   - M3-05's spec is written by M4-00, and M3-05 needs only M1-22.
   - New planning block M5-00a (needs M1-15) makes M5-01 and the installer core Linux-first.
   - A new S block M5-03b runs the installer on arc_os after M4-14.

## Consequences
- Estimated finish moves from about 44.5 to about 30.5 units (27.5 with item 6); total compute is about the same.
  These are estimates from the plan's own unit sizes, not measurements.
- Michael gets more PRs per day. Independent review and the WIP limit are the counterweights.
- More rebases, mostly on add-only shared files. A stacked child whose parent's interface changes in review
  redoes up to half a session of work.
- Rules out:
  - a third kernel lane (it buys about 1.5 units);
  - stacking across lanes or deeper than one;
  - running any kernel block beside M1-05;
  - loosening any test or review rule to go faster.

## Verification
- The coordinator's status table shows lane, stack depth and WIP count, and never more than the limits.
- Every D-tier kernel PR body links its independent review comment.
- Each smoke boot leaves a `docs/hardware/<machine>.md` or `docs/perf/` entry naming the commit.
- At M1-22, the M1 calendar is compared with this estimate. If two lanes produced more rework than they saved,
  go back to one lane.
