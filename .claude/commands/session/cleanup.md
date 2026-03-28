# /cleanup — Post-Completion Housekeeping

## Instructions

You are cleaning up after a completed feature or release. Development leaves
behind plan files, scratchpads, worktrees, and branches that served their
purpose but now clutter the repo. Remove or archive them so the next cycle
starts clean.

## Step 1: Identify Completed Work

1. Check scratchpad.md for the most recent feature
2. Check recent merge commits: `git log --merges --oneline -5`
3. Ask the user if ambiguous

## Step 2: Archive Plan Files

For each plan file related to the completed work:

```bash
mkdir -p docs/plans/archive
mv docs/plans/<completed-feature>.md docs/plans/archive/
```

Do NOT delete plan files — they're valuable historical records.

## Step 3: Clean Up Branches

```bash
git branch --merged main
```

List merged feature branches (not main). Confirm with the user before deleting.

## Step 4: Prune Worktrees

```bash
git worktree list
git worktree prune
```

For worktrees with no uncommitted changes, offer to remove.
For worktrees with uncommitted changes, warn the user.

## Step 5: Reset Scratchpads

- If `scratchpad.md` references the completed feature: replace with blank template
- If `scratchpad-<workstream>.md` files exist: archive alongside the plan, then delete

Blank template:

```markdown
# Scratchpad

## Current State
- Working on: <next task — to be filled>
- Branch: main
- Last completed step: —
- Next step: —

## Decisions Made During Implementation

## Open Questions

## Known Issues
```

## Step 6: Report

```
Cleanup complete:
- Plan archived: docs/plans/archive/<feature>.md
- Branches deleted: <list>
- Worktrees pruned: <list>
- Scratchpads reset: <list>
- Repo ready for next development cycle.
```
