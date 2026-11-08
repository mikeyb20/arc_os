# /os/implement-block <ID>

Run one work block from `docs/plans/work-blocks.md`, from spec to pull request, reading as little as possible.
The rules behind each step are in `docs/plans/work-blocks.md` §3.

## 1. Find the block
- `ls docs/plans/blocks/<ID>-*.md`. No file means the milestone's planning block (`M2-00a`, `M2-00` … `M5-00`) hasn't run
  yet: stop and say so.
- In the §4 table of `docs/plans/work-blocks.md`, check that every block in Needs is `done`. If not, stop and say
  which one is missing.

## 2. Load only what the block lists
- Read the block file, including Notes (a handoff from an earlier session if the block is half done).
- Read the files under "Read first". Nothing else up front: no `overview.md`, no other blocks, no ADRs the block
  doesn't cite, no repo-wide search. Open more files only when a step needs them.

## 3. Branch and mark it
- Branch `block/<id>-<short-name>` from master, or check out the existing branch if Notes say so.
- Set the block's Status to `doing` in the §4 table (part of the same PR).

## 4. Build, step by step
- Follow "Build" in order. After each step: build, run the host tests for what changed, run only the QEMU test for
  what changed (`ctest -L qemu -R <name>`).
- Read QEMU output by `ARC_TEST` markers or the tail. Never paste whole logs.
- Stay inside the block. Anything else goes into Notes as a follow-up.
- If the spec is wrong, fix the spec and say why in the PR. If a locked decision (ADR) is in the way, stop and ask
  Michael with a default.
- If the block passes 1.5× its size, stop, hand off (step 6) and propose a split.

## 5. Finish
- Every "Done when" box checked; the full `ctest -L qemu` run once.
- Update what the block changed: `syscall-table.md` rows, ADR links, later block specs that name a changed interface.
- Notes: 5–10 lines (what was done, decisions, surprises, anything the next block should know).
- Status `review` with the PR number. Open the PR (repo template, attribution); CI must be green.

## 6. Out of room before done
Commit `WIP: <block> — <state>` on the branch, write the exact next step in Notes, push. The next session runs
`/os/implement-block <ID>` and continues from Notes.
