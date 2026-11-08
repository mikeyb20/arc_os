# Block specs

One file per work block. The block list, order and status live in [`../work-blocks.md`](../work-blocks.md).
M0 and M1 have full specs here. M2–M5 get theirs from their planning block (`M2-00` … `M5-00`) just before
the milestone starts, because they depend on what M1 and M2 measure.

## Template

```markdown
# <ID> — <title>

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|

**Goal.** One or two sentences.

**Read first.** The only files the session reads before starting (plus CLAUDE.md, which loads itself).

**Build.** Numbered steps, in order. Each step leaves the tree building and its tests passing.

**Done when.** Checkable items. CI green is always implied.

**Cost.** (D blocks) Hot paths added, their target complexity, and bytes per object (ADR-0019).

**Watch out.** Known traps.

**Notes.** Filled in by the session: what was done, decisions taken, surprises, the exact next step if
unfinished, and anything the next block should know. 5–10 lines.
```

## Rules

- A spec is a contract for one session. If a step turns out wrong, fix the spec in the same PR and say why
  in the PR description.
- If a block changes an interface that a later block's spec names, update that later spec in the same PR.
- Notes are the handoff. `scratchpad*.md` is gitignored and cloud sessions are thrown away, so anything the next
  session needs goes in Notes and gets committed.
- Never add scope in a spec without Michael's OK (CLAUDE.md non-negotiables).
