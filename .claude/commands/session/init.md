# /init — Initialize Session Context

## Instructions

You are initializing a work session. Load all relevant context so the user
does not need to re-explain anything. Be concise in your summary — confirm
what you understand, flag anything unclear, and state what you'll do next.

## Step 1: Determine Workstream

Check if the user specified a workstream name (e.g., `/session/init workstream-a`).

If not specified:
1. Check the current git branch name — if it matches a workstream name from
   a plan file, use that workstream
2. If the plan defines multiple workstreams and the branch doesn't match,
   list the available workstreams and ask the user which one
3. If the plan has no workstreams (Tier 1), scope is the full feature

Do not guess which workstream to use. If ambiguous, ask.

## Step 2: Load State

1. Determine the correct scratchpad:
   - Workstream session: read `scratchpad-<workstream>.md` (if exists)
     AND `scratchpad.md` for global state
   - Single-workstream feature: read `scratchpad.md`
2. Identify the active plan: check scratchpad's "Working on" field, or
   find the most recently modified file in `docs/plans/`
3. Read the active plan file
4. Note current git branch: `git branch --show-current`
5. Note recent commits: `git log --oneline -5`

## Step 3: Determine Session Type

- **New implementation:** Plan exists but no work has started
- **Continuation:** Previous session made progress, picking up from last step
- **Parallel workstream:** This session owns a specific subset of files
- **Validation:** Post-build review (load diff instead of full plan)
- **Recovery:** Previous session hit an error or context limit

## Step 4: Summarize Context

Present to the user (keep under 10 lines):

```
Session initialized.
- Project: arc_os
- Working on: <feature from plan/scratchpad>
- Branch: <current branch>
- Workstream: <workstream name, or "full feature" if Tier 1>
- Last completed: <from scratchpad, or "fresh start">
- Next step: <from scratchpad, or first task in plan>
- Scope: <file list if parallel workstream, or "all files in plan">
```

## Step 5: Flag Issues

Before proceeding, check for:
- Uncommitted changes that might be from a crashed session
- Scratchpad open questions that need resolving
- Plan deviations noted in scratchpad that affect this session

If any issues found, present them and ask how to proceed before starting work.
