# Claude Code Skill Map — End-to-End Development Workflow v3

This document maps every phase of the development workflow (v2) to concrete
Claude Code skills. Each skill definition includes its trigger, context
requirements, expected outputs, workflow placement, and the actual command
specification agents will execute.

**Changelog from v2:**
- Reordered: `/review` now runs before `/done-check` (review finds bugs,
  done-check is the final gate after fixes)
- Merged: `/parallel-split` folded into `/plan` as an internal Tier 2 step
- Added: `/gen-tests` for autonomous test generation
- Fixed: per-workstream scratchpads for Tier 2 parallel work
- Sharpened: `/adversarial` no longer overlaps with `/review`
- Merged: `/escalate` absorbed into `/handoff` as an escalation mode
- Merged: `/doc-api` + `/doc-update` combined into `/docs`
- Fixed: `/fresh-eyes` reframed as "implementation-blind" not "zero context"
- Added: `/cleanup` for post-merge/release housekeeping
- Fixed: `/sweep` now includes a selection heuristic (not always 4 sweeps)
- Fixed: `/init` has explicit workstream parameter handling
- Fixed: `/plan-review` uses Sonnet for Tier 1, Opus for Tier 2

---

## Skill Inventory

19 skills organized into 7 categories. Each skill is a `.claude/commands/`
markdown file that agents invoke via slash commands.

```
.claude/
├── commands/
│   ├── planning/
│   │   ├── plan.md              # Create tiered plan (includes parallelism analysis for Tier 2)
│   │   └── plan-review.md       # Critique existing plan for gaps
│   │
│   ├── session/
│   │   ├── init.md              # Initialize session from plan + scratchpad
│   │   ├── handoff.md           # Persist state to scratchpad (includes escalation mode)
│   │   └── cleanup.md           # Post-merge/release housekeeping
│   │
│   ├── build/
│   │   ├── implement.md         # Implementation loop with inline validation
│   │   └── build-interfaces.md  # Contract-first interface scaffolding
│   │
│   ├── validate/
│   │   ├── review.md            # Post-build diff review against plan
│   │   ├── done-check.md        # Final gate: verify plan's done criteria are met
│   │   ├── sweep.md             # Focused validation sweep (with selection heuristic)
│   │   ├── adversarial.md       # Hostile environment and complex interaction testing
│   │   └── fresh-eyes.md        # Implementation-blind code assessment
│   │
│   ├── test/
│   │   └── gen-tests.md         # Generate tests for new or existing code
│   │
│   ├── integrate/
│   │   ├── predict-conflicts.md # Pre-merge conflict analysis
│   │   └── integrate-review.md  # Post-merge cross-workstream validation
│   │
│   ├── release/
│   │   ├── pre-release.md       # Pre-release audit checklist
│   │   ├── changelog.md         # Generate changelog from git history
│   │   └── catalog-debt.md      # Catalog and prioritize tech debt
│   │
│   └── docs/
│       ├── docs.md              # Generate/update inline + architecture documentation
│       └── doc-decision.md      # Record architectural decision (ADR)
```

---

## Workflow Map — Where Skills Fire

```
TIER 0: QUICK (1-2 files)
─────────────────────────
  [no skills — just do it]
  │
  ├── Escalation detected?
  │   └── YES → /handoff --escalate → /plan in new session
  │
  └── Done → commit

TIER 1: STANDARD (3-10 files)
─────────────────────────────
  /plan ─────────────────────────── Create lightweight plan
  │
  /plan-review ──────────────────── Agent critiques plan (Sonnet)
  │
  /init ─────────────────────────── Load plan + scratchpad into session
  │
  /implement ────────────────────── Build loop: implement → test → self-review → commit
  │   │
  │   ├── Plan wrong? → /handoff → update plan → /init new session
  │   ├── Context full? → /handoff → /init new session
  │   └── Scope grew? → /handoff --escalate → /plan (Tier 2) in new session
  │
  /gen-tests ────────────────────── Generate tests for new code (can run in parallel)
  │
  /review ───────────────────────── Fresh session diff review
  │   │
  │   └── Issues found? → fix → re-run /review
  │
  /done-check ───────────────────── Final gate: verify done criteria
  │
  /docs ─────────────────────────── Inline API docs + architecture doc sync
  │
  merge → run full tests → /cleanup → done

TIER 2: COMPLEX (10+ files, parallel)
──────────────────────────────────────
  /plan (Tier 2 mode) ──────────── Create plan with workstreams + parallelism analysis
  │
  /plan-review ──────────────────── Agent critiques plan (Opus)
  │
  /doc-decision ─────────────────── Record architectural decisions from planning
  │
  /build-interfaces ─────────────── Build shared types/traits/contracts first
  │   │
  │   └── commit interfaces → base point for all worktrees
  │
  ┌─── Worktree A ──────────────┐  ┌─── Worktree B ──────────────┐
  │ /init workstream-a          │  │ /init workstream-b          │
  │ /implement                  │  │ /implement                  │
  │   ├── /handoff (on pause)   │  │   ├── /handoff (on pause)   │
  │   └── /init (on resume)     │  │   └── /init (on resume)     │
  │ /gen-tests                  │  │ /gen-tests                  │
  │ /review                     │  │ /review                     │
  │ /done-check                 │  │ /done-check                 │
  │ /docs                       │  │ /docs                       │
  └─────────────────────────────┘  └─────────────────────────────┘
  │
  /predict-conflicts ────────────── Analyze branches for merge risk
  │
  merge (ordered: producers → consumers)
  │
  /integrate-review ─────────────── Cross-workstream integration validation
  │
  /sweep (selected types) ──────── 1-2 sweeps based on change type, not all 4
  │
  /adversarial ──────────────────── Hostile environment + complex interaction testing
  │
  /fresh-eyes ───────────────────── Implementation-blind assessment (optional, high-value)
  │
  /docs ─────────────────────────── Final doc sync with merged codebase
  │
  /pre-release ──────────────────── Audit checklist before release
  │
  /changelog ────────────────────── Generate changelog from commits
  │
  tag + release
  │
  /catalog-debt ─────────────────── Post-release debt backlog
  │
  /cleanup ──────────────────────── Archive plans, prune worktrees, reset scratchpads
```

---

## Skill Definitions

Each definition below follows this structure:
- **Trigger**: When this skill fires (the description field for auto-activation)
- **Context required**: What the agent needs access to before executing
- **Outcome**: What the skill produces (artifacts, files, decisions)
- **Workflow phase**: Where in the tier this skill belongs
- **Model**: Recommended model for this skill
- **Command spec**: The actual prompt template for the `.md` file

---

### PLANNING SKILLS

---

#### `/plan` — Create Development Plan

**Trigger:** User wants to plan a new feature, refactor, or task before implementation.
Keywords: "plan", "scope", "spec", "design", "architect", "break down".

**Context required:**
- User description of the feature or task
- Access to existing codebase for impact analysis
- CLAUDE.md for project conventions and architecture

**Outcome:**
- `docs/plans/<feature-name>.md` — complete plan file following the tier-appropriate
  template (lightweight for Tier 1, full with workstreams and parallelism analysis for Tier 2)
- All architectural decisions are explicit — no ambiguity left for implementation
- Done criteria are specific and testable
- For Tier 2: validated workstream breakdown with file ownership, dependency graph,
  conflict risk assessment, and merge order

**Workflow phase:** First step in Tier 1 and Tier 2. Not used in Tier 0.

**Model:** Opus

**Command spec (`planning/plan.md`):**

```markdown
# /plan — Create Development Plan

## Instructions

You are creating a development plan. Your goal is to produce a plan file that
eliminates all ambiguity before implementation begins. Every architectural
decision must be made explicitly in this plan — the implementation agent should
never need to guess.

## Step 1: Assess Tier

Determine the scope by analyzing the user's description:
- **Tier 1 (Standard):** 3-10 files, single concern, one workstream
- **Tier 2 (Complex):** 10+ files, multiple concerns, cross-cutting, or
  requires parallel workstreams

If the task is 1-2 files with an obvious fix, tell the user this doesn't
need a plan — just implement it directly.

## Step 2: Gather Context

Before writing the plan:
1. Read CLAUDE.md for project conventions and architecture
2. Explore the codebase areas relevant to the user's description
3. Identify all files that will need to change
4. Identify all interfaces (types, function signatures, API contracts) that
   will be created, modified, or consumed

Ask the user clarifying questions if you cannot determine:
- The intended approach or algorithm
- Which existing patterns to follow
- How errors should be handled
- What the boundaries of the change are

## Step 3: Write Plan

Create `docs/plans/<feature-name>.md` using the appropriate template.

### Tier 1 Template:

```
# <Feature Name>

## What
<One sentence describing the deliverable>

## Why
<What problem this solves — prevents scope creep later>

## Boundaries
<What this change does NOT touch — be explicit>

## Files to Create/Modify
- <path>: <what changes and why>

## Interfaces
<Function signatures, types, or API contracts introduced or changed>

## Done Criteria
- [ ] <Specific, testable condition>
- [ ] All existing tests pass
- [ ] New tests cover <specific scenarios>

## Test Approach
<What to test, which framework, TDD or post-implementation>
```

### Tier 2 Template:

Includes everything in Tier 1, plus the parallelism analysis sections below.

```
## Architecture Impact
<How this changes the system structure, new dependencies, affected modules>

## Workstream Breakdown

### Shared Interfaces (build first)
- <type/interface>: <file path> — <description>

### Workstream A: <name>
- **Files:** <exact list of files this workstream owns>
- **Depends on:** <interfaces from shared or other workstreams>
- **Exposes:** <interfaces other workstreams consume>
- **Estimated complexity:** <low/medium/high>

### Workstream B: <name>
<same structure>

## Dependency Graph
<Which workstreams depend on which, in what direction.
  Producer workstreams must complete before consumers begin.>

## Conflict Risk Assessment
For each pair of workstreams, rate the conflict risk:
- NONE: completely independent file sets
- LOW: shared read dependencies only
- MEDIUM: one produces interfaces the other consumes (ordered merge resolves)
- HIGH: both need to modify the same file (must serialize or redesign)

Specific conflicts:
- <File/module> touched by workstreams <A> and <B> — resolution: <who owns it>
- <Shared config/build files> — resolution: <which workstream owns>

If any HIGH conflict risks exist, redesign the split or serialize those tasks.
Do NOT approve a parallel split with unresolved HIGH conflict risks.

## Merge Order
1. Shared interfaces
2. <Producer workstream> — rationale: <why this merges first>
3. <Consumer workstream> — rationale: <depends on producer>
4. <Test/docs workstream> — rationale: <needs final codebase state>
```

## Step 4: Parallelism Validation (Tier 2 only)

For Tier 2 plans, validate the workstream breakdown before presenting:

### 4a. Build File Dependency Graph
For every file listed in the plan:
1. Classify as CREATE (new file) or MODIFY (existing file)
2. For MODIFY: identify what is being changed (types, functions, imports, config)
3. Map which files import/include/depend on which other files
4. Identify shared files: types, configs, build files, utilities

### 4b. Verify Independence
Confirm that:
- No two workstreams WRITE to the same file
- If workstream A reads a file that workstream B writes, A depends on B
  and the merge order reflects this
- Shared type definition files are owned by ONE workstream, read-only for others
- Build system files (CMakeLists, Cargo.toml, package.json) are owned by ONE
  workstream, typically the one adding dependencies
- If a file MUST be written by two workstreams, those tasks are serialized

### 4c. Define Contracts
For each workstream boundary, verify:
- Interface: exact type/function signatures exchanged between workstreams
- Direction: which workstream produces, which consumes
- Build order: producer must complete and commit before consumer begins

## Step 5: Self-Review

Before presenting the plan, verify:
- [ ] Every file that will be touched is listed explicitly
- [ ] No interface is consumed by one workstream but not produced by any
- [ ] Done criteria are testable (not vague like "works correctly")
- [ ] Boundaries are stated — what is explicitly OUT of scope
- [ ] For Tier 2: workstream file lists don't overlap (no write conflicts)
- [ ] For Tier 2: merge order respects dependency direction
- [ ] For Tier 2: no unresolved HIGH conflict risks

## Step 6: Present for Review

Present the plan to the user. Explicitly call out:
- Decisions you made that the user should confirm
- Risks or uncertainties you identified
- Alternative approaches you considered and why you chose this one

Do NOT begin implementation. Wait for user approval.
```

---

#### `/plan-review` — Critique Existing Plan

**Trigger:** User has a plan file and wants it reviewed for gaps, risks, or
improvements before implementation. Keywords: "review plan", "check plan",
"critique", "what am I missing".

**Context required:**
- The plan file at `docs/plans/<n>.md`
- Access to codebase for validating file lists and interface claims

**Outcome:**
- Numbered list of issues found, categorized by severity (blocking / should-fix / nice-to-have)
- Specific recommendations for each issue
- Confirmation that the plan is ready for implementation, or list of items to resolve first

**Workflow phase:** After `/plan`, before implementation begins.

**Model:** Sonnet for Tier 1 plans. Opus for Tier 2 plans (parallelism validation
and cross-workstream dependency analysis benefits from deeper reasoning).

**Command spec (`planning/plan-review.md`):**

```markdown
# /plan-review — Critique Development Plan

## Instructions

You are reviewing a development plan as a senior engineer. Your job is to find
gaps, risks, and ambiguities that will cause problems during implementation.
You are not being helpful by approving a weak plan — you are being helpful by
catching problems now when they're cheap to fix.

## Step 1: Load Plan

Read the plan file. If no path is specified, check `docs/plans/` for the most
recently modified plan, or ask the user which plan to review.

Determine whether this is a Tier 1 (no workstreams) or Tier 2 (workstreams) plan.

## Step 2: Validate Completeness

Check that the plan includes:
- [ ] Clear one-sentence description of the deliverable
- [ ] Explicit boundaries (what is NOT in scope)
- [ ] Complete file list (explore the codebase to verify nothing is missing)
- [ ] Interface definitions for all public contracts
- [ ] Testable done criteria (not vague)
- [ ] Test approach specified

For Tier 2 plans, additionally check:
- [ ] Workstream file lists are non-overlapping for write operations
- [ ] Every consumed interface has a producing workstream
- [ ] Dependency graph is acyclic and complete
- [ ] Merge order respects dependency direction
- [ ] Conflict risk assessment covers all shared files
- [ ] No unresolved HIGH conflict risks
- [ ] Shared interfaces section lists everything needed before parallel work

## Step 3: Validate Against Codebase

Explore the actual codebase to verify:
- Do the files listed actually exist (for modifications)?
- Are there files the plan missed that will need changes?
- Do the interfaces match existing patterns in the codebase?
- Are the named modules/types/functions spelled correctly?
- Does the architecture impact assessment match reality?

## Step 4: Identify Risks

Look for:
- Decisions the plan defers to implementation (these become ambiguity)
- Implicit assumptions that aren't stated
- Cross-cutting concerns not addressed (logging, error handling, config)
- Migration or backward-compatibility issues
- Performance implications for hot paths

## Step 5: Report

Present findings as:

### Blocking Issues (must fix before implementation)
<numbered list with specific recommendation for each>

### Should Fix (will cause pain later if ignored)
<numbered list with specific recommendation for each>

### Suggestions (improvements, not required)
<numbered list>

### Verdict
<READY FOR IMPLEMENTATION / NEEDS REVISION — with summary of what to fix>
```

---

### SESSION LIFECYCLE SKILLS

---

#### `/init` — Initialize Session

**Trigger:** Starting a new implementation, validation, or continuation session
that needs project context. Keywords: "start", "begin", "continue",
"pick up where I left off", "resume".

**Usage:** `/init` (auto-detect) or `/init workstream-a` (specific workstream)

**Context required:**
- `scratchpad.md` or `scratchpad-<workstream>.md` (if exists)
- Active plan file at `docs/plans/<n>.md` (if exists)
- CLAUDE.md (auto-loaded)
- Workstream name (from user argument, branch name, or user selection)

**Outcome:**
- Agent has full context of current state, active plan, and assigned scope
- No re-explanation needed from the user
- Clear understanding of what to do next

**Workflow phase:** Start of every non-trivial session across all tiers.

**Model:** Any (context loading, not reasoning-heavy)

**Command spec (`session/init.md`):**

```markdown
# /init — Initialize Session Context

## Instructions

You are initializing a work session. Load all relevant context so the user
does not need to re-explain anything. Be concise in your summary — confirm
what you understand, flag anything unclear, and state what you'll do next.

## Step 1: Determine Workstream

Check if the user specified a workstream name (e.g., `/init workstream-a`).

If not specified:
1. Check the current git branch name — if it matches a workstream name from
   a plan file, use that workstream
2. If the plan defines multiple workstreams and the branch doesn't match,
   list the available workstreams and ask the user which one this session is for
3. If the plan has no workstreams (Tier 1), scope is the full feature

Do not guess which workstream to use. If ambiguous, ask.

## Step 2: Load State

1. Determine the correct scratchpad:
   - If this is a workstream session: read `scratchpad-<workstream>.md` (if exists)
     AND `scratchpad.md` for global state
   - If this is a single-workstream feature: read `scratchpad.md`
2. Identify the active plan: check scratchpad's "Working on" field, or
   find the most recently modified file in `docs/plans/`
3. Read the active plan file
4. Note current git branch: `git branch --show-current`
5. Note recent commits: `git log --oneline -5`

## Step 3: Determine Session Type

Based on scratchpad state and user input, determine:
- **New implementation:** Plan exists but no work has started
- **Continuation:** Previous session made progress, picking up from last step
- **Parallel workstream:** This session owns a specific subset of files
- **Validation:** Post-build review (load diff instead of full plan)
- **Recovery:** Previous session hit an error or context limit

## Step 4: Summarize Context

Present to the user (keep under 10 lines):

```
Session initialized.
- Project: <name from CLAUDE.md>
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
- Context window budget (report current usage if available)

If any issues found, present them and ask how to proceed before starting work.
```

---

#### `/handoff` — Persist Session State

**Trigger:** Ending a session, pausing work, context window approaching limit,
switching to a different task, or escalating to a higher tier. Keywords: "stop",
"pause", "done for now", "save state", "hand off", "context full", "escalate",
"this is bigger than I thought".

**Usage:** `/handoff` (normal) or `/handoff --escalate` (tier escalation)

**Context required:**
- Current session's full context (what was done, decisions made)
- Existing scratchpad (to update, not overwrite)
- Active plan file (to check progress against)

**Outcome:**
- Scratchpad updated with current state, decisions, next steps, and known issues
- All work committed to git with descriptive messages
- Next session can `/init` and continue seamlessly
- If escalating: WIP committed, escalation reason recorded, guidance to run
  `/plan` in a fresh session at the higher tier

**Workflow phase:** End of every implementation session. Triggered by user or
when context exceeds 50%. Also handles tier escalation (previously `/escalate`).

**Model:** Any

**Command spec (`session/handoff.md`):**

```markdown
# /handoff — Persist Session State

## Instructions

You are saving the current session's state so a future session (possibly with
no shared context) can continue exactly where you left off. Be precise and
complete — anything you don't write down is lost.

If the user specified `--escalate`, this is a tier escalation — the task has
grown beyond its original scope and needs a higher-tier plan before continuing.

## Step 1: Commit Pending Work

1. Check `git status` for uncommitted changes
2. If changes exist: stage and commit with a descriptive message
   - If changes are incomplete/WIP: commit with prefix "WIP: <description>"
   - If escalating: commit with "WIP: pausing for tier escalation — <reason>"
3. If no changes: note this (session may have been read-only)

## Step 2: Assess Progress

Compare current state against the plan at `docs/plans/<feature>.md`:
- Which tasks/steps from the plan are complete?
- Which task is currently in progress?
- What is the exact next step to continue?

## Step 3: Assess Escalation (if --escalate)

If escalating, additionally determine:
- How many files will this task actually touch? (revised estimate)
- Are there multiple independent concerns that could be parallelized?
- Are there shared interfaces that need to be defined first?
- What was learned during implementation that changes the approach?
- Which existing work should be kept, rebased, or discarded?

Recommend the appropriate tier:
- **Tier 1:** 3-10 files, single workstream, needs a lightweight plan
- **Tier 2:** 10+ files or multiple concerns, needs full plan with workstreams

## Step 4: Update Scratchpad

Determine the correct scratchpad file:
- If this is a workstream session: update `scratchpad-<workstream>.md`
- If this is a single-workstream feature: update `scratchpad.md`

Update the scratchpad (create if it doesn't exist). Preserve existing
content in the Decisions and Known Issues sections — append, don't overwrite.

```markdown
# Scratchpad

## Current State
- Working on: <feature name>
- Plan: docs/plans/<feature>.md
- Branch: <branch name>
- Workstream: <workstream name, or "full feature">
- Last completed step: <specific description of what was just finished>
- Next step: <specific description of what should happen next>
- Context usage at handoff: <percentage if known>
- Handoff reason: <context full / pausing / escalating / complete>

## Decisions Made During Implementation
<preserve all existing entries>
- <any new decisions from this session>: <rationale>

## Open Questions
<preserve unresolved, remove resolved>
- <any new questions discovered>

## Known Issues
<preserve existing>
- <any new issues discovered during this session>
```

If escalating, append:

```markdown
## Escalation Note
- Escalated from Tier <X> to Tier <Y> at <timestamp>
- Reason: <why the original scope was insufficient>
- Existing work on branch <branch>: <keep / discard / partial — with details>
- Revised file count estimate: <N files>
- Key learnings to carry into new plan:
  - <what was discovered during implementation>
  - <decisions that should be preserved>
  - <approaches that failed and why>
```

## Step 5: Confirm

Report to user:
- Files committed (or "no changes")
- Scratchpad updated at <path>
- Ready for session end

If escalating, additionally advise:
1. What tier the work should escalate to
2. Whether existing work should be kept, rebased, or discarded
3. Recommended next step: "Start a fresh session and run `/plan` referencing
   the scratchpad for context from this attempt"

Do NOT continue implementation after this skill runs. The session is ending.
```

---

#### `/cleanup` — Post-Completion Housekeeping

**Trigger:** Feature has been merged or released and the repo has accumulated
debris from the development process. Keywords: "cleanup", "clean up",
"tidy up", "archive", "prune".

**Context required:**
- Current git state (branches, worktrees)
- `docs/plans/` directory
- Scratchpad files
- `.claude/worktrees/` directory

**Outcome:**
- Completed plan files archived to `docs/plans/archive/`
- Merged feature branches deleted
- Stale worktrees pruned
- Scratchpad files reset or archived
- Repo is clean and ready for the next development cycle

**Workflow phase:** After merge (Tier 1) or after release (Tier 2).

**Model:** Sonnet (mechanical task)

**Command spec (`session/cleanup.md`):**

```markdown
# /cleanup — Post-Completion Housekeeping

## Instructions

You are cleaning up after a completed feature or release. Development leaves
behind plan files, scratchpads, worktrees, and branches that served their
purpose but now clutter the repo. Remove or archive them so the next cycle
starts clean.

## Step 1: Identify Completed Work

Determine what was just completed:
1. Check scratchpad.md for the most recent feature
2. Check recent merge commits: `git log --merges --oneline -5`
3. Ask the user if ambiguous

## Step 2: Archive Plan Files

For each plan file related to the completed work:

```bash
mkdir -p docs/plans/archive
mv docs/plans/<completed-feature>.md docs/plans/archive/
```

Do NOT delete plan files — they're valuable historical records of decisions.

## Step 3: Clean Up Branches

```bash
# List merged branches
git branch --merged main

# For each merged feature branch (not main, not develop):
git branch -d <branch>
```

Confirm with the user before deleting any branches. List them first.

## Step 4: Prune Worktrees

```bash
# List all worktrees
git worktree list

# Remove stale worktree metadata
git worktree prune

# Check for leftover worktree directories
ls .claude/worktrees/ 2>/dev/null
```

For any remaining worktree directories with no uncommitted changes,
offer to remove them. For worktrees with uncommitted changes, warn the user.

## Step 5: Reset Scratchpads

For the completed feature:
- If `scratchpad.md` references the completed feature: clear its contents
  and replace with a blank template ready for the next task
- If `scratchpad-<workstream>.md` files exist for completed workstreams:
  archive them to `docs/plans/archive/` alongside the plan, then delete

Blank scratchpad template:

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
```

---

### BUILD SKILLS

---

#### `/implement` — Implementation Loop

**Trigger:** User is ready to implement from an approved plan. This is the
core build loop. Keywords: "implement", "build", "code it", "start building",
"execute the plan".

**Context required:**
- Approved plan at `docs/plans/<n>.md`
- Scratchpad state (if continuing)
- CLAUDE.md for conventions
- Scope constraints (if parallel workstream)

**Outcome:**
- Feature implemented according to plan
- Each subtask committed individually with descriptive messages
- Tests passing after each commit
- Inline self-review performed before each commit
- Scratchpad updated on pause or completion

**Workflow phase:** Core of Tier 1 and Tier 2 implementation phases.

**Model:** Sonnet (Tier 1) or Opus (Tier 2 complex workstreams)

**Command spec (`build/implement.md`):**

```markdown
# /implement — Implementation Loop

## Instructions

You are implementing a feature from an approved plan. Follow the plan precisely.
Do not make architectural decisions that the plan doesn't address — if you
encounter an ambiguity, STOP and ask the user rather than guessing.

## Step 1: Load Context

1. Read the plan at the specified path (or from scratchpad's active plan)
2. Read the appropriate scratchpad for current state and any decisions already made:
   - Workstream session: `scratchpad-<workstream>.md` + `scratchpad.md`
   - Single workstream: `scratchpad.md`
3. Determine your scope:
   - If a workstream is specified: you may ONLY modify files listed for that workstream
   - If no workstream: you may modify any file listed in the plan
   - Files not listed in the plan are OFF LIMITS unless you get user approval

## Step 2: Implementation Loop

For each task in the plan (or continuing from scratchpad's "next step"):

### 2a. Implement
Write the code for this task. Follow conventions from CLAUDE.md.
Reference existing patterns in the codebase for consistency.

### 2b. Build + Test
Run the project's build command and test suite.
- Build fails → fix immediately before proceeding
- Tests fail → fix immediately before proceeding
- Do not move to the next task with a broken build

### 2c. Self-Review (before committing)
Review your own changes for this task:
- Edge cases: what inputs or states could cause this to fail?
- Error handling: are all failure paths handled? No silent failures?
- Plan compliance: does this match what the plan specified?
- Scope: did you modify any files outside your scope? If yes, revert those changes.
- Naming: do names match project conventions?

If self-review finds issues, fix them before committing.

### 2d. Commit
Commit with a descriptive message following project conventions.
Message format: "<type>: <what changed and why>"
Examples:
- "feat: add pathfinding A* implementation for ground units"
- "fix: handle empty input in resource parser"
- "refactor: extract shared validation logic to utils"

### 2e. Progress Check
- If context usage > 50%: run /handoff and advise user to start fresh session
- If the plan needs to change: STOP, explain what you've discovered,
  and ask user whether to update the plan or proceed differently
- If scope has grown beyond the current tier: advise user to run
  /handoff --escalate

## Step 3: Completion

When all tasks in scope are complete:
1. Run full test suite one final time
2. Update the appropriate scratchpad: mark as
   "Implementation complete, ready for validation"
3. Report to user: what was built, how many commits, any deviations from plan

## CRITICAL RULES

- NEVER modify files outside your declared scope without user approval
- NEVER make architectural decisions the plan doesn't cover — ask instead
- NEVER continue past a failing build or test suite
- ALWAYS commit after each completed subtask, not in bulk at the end
- If you discover the plan is wrong: STOP and say so. Do not silently diverge.
```

---

#### `/build-interfaces` — Contract-First Interface Scaffolding

**Trigger:** Tier 2 plan has shared interfaces that must exist before parallel
workstreams begin. Keywords: "build interfaces", "create contracts",
"scaffold interfaces", "set up shared types".

**Context required:**
- Tier 2 plan with "Shared Interfaces" section
- Codebase for consistency with existing patterns

**Outcome:**
- All shared types, traits, function signatures, and API contracts implemented
- Stub/placeholder bodies where full implementation comes later
- Committed as the base point that all worktrees branch from
- Documentation comments on all public interfaces

**Workflow phase:** After plan approval, before parallel worktrees launch. Tier 2 only.

**Model:** Opus

**Command spec (`build/build-interfaces.md`):**

```markdown
# /build-interfaces — Contract-First Interface Scaffolding

## Instructions

You are building shared interfaces that parallel workstreams will depend on.
These are the contracts between workstreams — they must be correct, complete,
and stable. Once committed, implementation workstreams treat these files as
read-only.

## Step 1: Load Plan

Read the Tier 2 plan. Extract the "Shared Interfaces" section.
For each interface listed, gather:
- Type/trait/interface name
- File path where it will live
- Which workstreams produce it and which consume it
- Expected behavior contract

## Step 2: Implement Interfaces

For each shared interface:

1. Create the file at the specified path
2. Implement the type definitions, trait declarations, and function signatures
3. Add documentation comments explaining:
   - What this interface represents
   - Which workstreams produce and consume it
   - Invariants that implementations must maintain
   - Example usage (for complex interfaces)
4. Where full implementation is deferred to a workstream:
   - Use `todo!()` / `unimplemented!()` (Rust)
   - Use stub bodies with `// TODO: implemented by workstream-X` (C++)
   - Use `throw new Error('Not implemented')` (TypeScript)

## Step 3: Validate Contracts

For each interface, verify:
- [ ] All types referenced in the plan exist
- [ ] All function signatures match the plan
- [ ] No circular dependencies between interface files
- [ ] Documentation comments are present on all public items
- [ ] The code compiles/type-checks (stubs are fine, type errors are not)

## Step 4: Commit

```bash
git add <interface files>
git commit -m "feat: add shared interfaces for <feature name>

Contracts for parallel workstreams:
- <interface 1>: <purpose>
- <interface 2>: <purpose>

These files are read-only for implementation workstreams."
```

## Step 5: Report

Tell the user:
- Interfaces committed on current branch
- Each worktree should branch from this commit
- List which interfaces each workstream depends on and produces
- Ready to launch parallel sessions
```

---

### VALIDATION SKILLS

---

#### `/review` — Post-Build Diff Review

**Trigger:** Implementation is complete and needs review before done-check.
MUST be run in a fresh session, not the session that wrote the code.
Keywords: "review", "check my code", "code review", "pre-merge review".

**Context required:**
- Git diff between feature branch and main
- Plan file for spec compliance checking
- Scratchpad "Decisions Made" section for documented deviations
- NO context from the implementation session (fresh session mandatory)

**Outcome:**
- List of issues found, categorized by severity
- Spec compliance report (plan vs. actual)
- Explicit pass/fail recommendation
- If REVISE: specific items that must be addressed before re-review

**Workflow phase:** After implementation, before `/done-check`. All tiers except Tier 0.
Run this first. Fix issues. Then run `/done-check` as the final gate.

**Model:** Opus

**Command spec (`validate/review.md`):**

```markdown
# /review — Post-Build Diff Review

## Instructions

You are reviewing code you did NOT write. You have no context from the
implementation session. This is intentional — you catch things the
building agent is blind to because you don't share its assumptions.

Focus on correctness and robustness. Do NOT comment on style unless
it impacts readability or correctness.

## Step 1: Load Context

1. Identify the feature branch: `git branch --show-current`
2. Load the diff: `git diff main..HEAD`
3. Read the plan: check scratchpad(s) for the plan path,
   or find it in `docs/plans/`
4. Read CLAUDE.md for project conventions

## Step 2: Spec Compliance

For each item in the plan:
- Is it implemented? (yes / no / partial)
- Does the implementation match the specified approach?
- Are the interfaces exactly as defined in the plan?

Flag any deviations. Check scratchpad "Decisions Made" section —
documented deviations are acceptable if the rationale is sound.
Undocumented deviations are findings.

## Step 3: Correctness Review

For each file in the diff:

### Error Handling
- Are all error paths handled?
- Any panics, unwraps, or bare exceptions that should be Results/Options?
- Any silent failures (errors caught but not logged or propagated)?

### Edge Cases
- Empty/null/zero inputs
- Boundary values (max int, empty collections, single element)
- Concurrent access (if applicable)
- Resource cleanup on error paths

### Safety (language-specific)
- **C++:** undefined behavior, dangling references, buffer overflows,
  uninitialized memory, missing virtual destructors
- **Rust:** unsafe blocks justified?, lifetime correctness, unwrap audit
- **Go:** unchecked errors, goroutine leaks, nil pointer risks
- **TypeScript:** any casts, null assertions, type narrowing gaps

### Logic
- Do conditionals cover all cases?
- Are loop bounds correct?
- Are off-by-one errors present?
- Do mathematical operations handle overflow/underflow?

## Step 4: Report

```markdown
## Review: <feature name>

### Spec Compliance
<pass/fail per plan item, with notes on documented deviations>

### Issues Found

#### Critical (must fix before merge)
<numbered list with file:line, description, and fix recommendation>

#### Important (should fix)
<numbered list>

#### Minor (optional improvements)
<numbered list>

### Verdict: <APPROVE / REVISE>
```

If REVISE: list specific items that must be addressed. After fixes are made,
a new `/review` session should be run before proceeding to `/done-check`.
```

---

#### `/done-check` — Verify Done Criteria

**Trigger:** After `/review` has passed (or issues from review have been fixed),
this is the final gate before merge. Keywords: "done check", "are we done",
"verify criteria", "check completion", "final gate".

**Context required:**
- Plan file with done criteria
- Current codebase state (post-review fixes)
- Test results

**Outcome:**
- Per-criterion pass/fail with evidence
- Clear go/no-go for merge

**Workflow phase:** After `/review` passes, immediately before merge.
All tiers except Tier 0. This is a gate — if it fails, do not merge.

**Model:** Opus

**Command spec (`validate/done-check.md`):**

```markdown
# /done-check — Verify Done Criteria (Final Gate)

## Instructions

You are the final gate before merge. Every done criterion from the plan must
be verified. This runs AFTER code review — the code should already be correct.
Your job is to confirm completeness, not find bugs (that was /review's job).

If any criterion fails, the feature is not ready to merge. Be rigorous.
"Probably works" is not a pass.

## Step 1: Load Criteria

Read the plan file and extract the "Done Criteria" section.
Each criterion should be a testable statement.

## Step 2: Verify Each Criterion

For each criterion:

1. **Determine how to verify it:**
   - Can it be checked by reading code? → read the relevant files
   - Can it be checked by running tests? → run them and report results
   - Can it be checked by running the application? → note it requires manual check
   - Can it be checked by inspecting git history? → check commits

2. **Execute the verification**

3. **Record the result:**
   - ✅ **PASS** — criterion is met, with evidence
   - ❌ **FAIL** — criterion is not met, with explanation of what's missing
   - ⚠️ **MANUAL** — requires human verification (e.g., "UI looks correct")

## Step 3: Run Full Test Suite

Execute the project's test command. Report:
- Total tests: <count>
- Passing: <count>
- Failing: <count> (with details)
- Skipped: <count>

## Step 4: Report

```markdown
## Done Check: <feature name> (Final Gate)

| # | Criterion | Status | Evidence |
|---|-----------|--------|----------|
| 1 | <criterion> | ✅ PASS | <evidence> |
| 2 | <criterion> | ❌ FAIL | <what's missing> |

### Test Suite
<pass/fail summary>

### Verdict: <READY TO MERGE / NOT READY>

### Remaining Items (if not ready)
<specific list of what needs to happen before re-running done-check>
```
```

---

#### `/sweep` — Focused Validation Sweep

**Trigger:** Need a targeted analysis pass on the codebase. Run as a dedicated
session for each sweep type. Keywords: "safety sweep", "architecture review",
"performance check", "find duplicates", "audit".

**Context required:**
- Codebase (or specific files/modules to sweep)
- Sweep type parameter: safety | architecture | performance | duplication
- CLAUDE.md for project-specific patterns
- Plan file (for understanding what type of change was made)

**Outcome:**
- Categorized findings specific to the sweep type
- Prioritized by severity and blast radius
- Actionable recommendations (not just "this looks bad")

**Workflow phase:** Post-merge validation in Tier 2. Also useful for periodic
maintenance and debt paydown. NOT always 4 sweeps — use the selection heuristic
to pick 1-2 relevant sweep types.

**Model:** Opus

**Command spec (`validate/sweep.md`):**

```markdown
# /sweep — Focused Validation Sweep

## Instructions

You are running a focused validation sweep. Examine ONLY the specified
concern — do not mix concerns. A safety sweep does not comment on
architecture. A performance sweep does not flag naming issues.

## Step 0: Sweep Selection (if type not specified)

If the user hasn't specified which sweep type to run, recommend based on the
type of change. Read the plan file and recent diff to determine:

| Change Type | Recommended Sweeps |
|------------|-------------------|
| New code (new modules, features) | safety + architecture |
| Refactor (restructuring existing code) | architecture + duplication |
| Performance-sensitive (game loops, hot paths, high-throughput) | performance |
| Multi-workstream merge | duplication (cross-workstream duplicate detection) |
| C++ or unsafe Rust code | safety (always) |
| Bug fix only | safety (focused on the fix area) |

Recommend 1-2 sweep types. The user may override. Do NOT default to all 4 —
that's only appropriate for major releases or comprehensive audits.

## Sweep Types

### safety
Focus: memory safety, undefined behavior, panics, error handling completeness.

Examine:
- **C++:** UB (signed overflow, null deref, dangling refs, buffer access),
  uninitialized variables, missing virtual destructors, raw pointer usage
  in non-RAII contexts, exception safety guarantees
- **Rust:** unwrap/expect without justification, unsafe blocks (are they
  necessary? are invariants documented?), panic paths in library code,
  error types that lose context
- **Go:** unchecked errors (especially from Close, Write), nil pointer
  risks, goroutine leaks, race conditions
- **TypeScript:** any casts, non-null assertions (!), uncaught promise
  rejections, type narrowing that could fail at runtime

### architecture
Focus: module boundaries, dependency direction, abstraction quality.

Examine:
- Does the module structure match the documented architecture?
- Are there circular dependencies?
- Are abstractions at the right level? (too thin = wrapper noise, too thick = god objects)
- Does data flow in the expected direction? (e.g., UI → service → data, not reverse)
- Are there layering violations? (e.g., data layer importing UI types)
- Have modules grown beyond their stated responsibility?

### performance
Focus: unnecessary allocations, algorithmic complexity, cache behavior.

Examine:
- Allocations in hot loops (per-frame in games, per-request in servers)
- Algorithmic complexity mismatches (O(n²) where O(n log n) is achievable)
- Unnecessary copies (especially large structs)
- Cache-unfriendly access patterns (pointer chasing, random access on large arrays)
- Missing batching opportunities
- Database N+1 query patterns
- Blocking operations on async paths

### duplication
Focus: repeated logic, near-duplicate code, extraction opportunities.

Examine:
- Functions/methods that do nearly the same thing with slight variations
- Copy-pasted code blocks (especially across workstream boundaries after merge)
- Repeated error handling patterns that should be abstracted
- Configuration or constants duplicated across files
- Test setup code that should be shared fixtures

## Output Format

```markdown
## <Sweep Type> Sweep: <scope description>

### Critical (high blast radius, likely to cause issues)
1. **<file:line>** — <finding>
   - Impact: <what goes wrong>
   - Fix: <specific recommendation>

### Important (moderate risk)
<same format>

### Minor (low risk, worth addressing when nearby)
<same format>

### Summary
- <N> critical, <N> important, <N> minor findings
- Highest priority: <top 3 items to fix first>
```
```

---

#### `/adversarial` — Hostile Environment and Complex Interaction Testing

**Trigger:** Want to stress-test code against scenarios that go beyond basic
correctness — hostile environments, complex interaction sequences, and
emergent failure modes that simple edge-case checking (done in `/review`) 
would not catch. Keywords: "break this", "adversarial", "stress test",
"what could go wrong in production", "attack surface".

**Context required:**
- Feature code (specific files or recent diff)
- Plan/spec for understanding intended behavior
- NO implementation session context (fresh session)

**Outcome:**
- Concrete reproduction cases for each failure mode found
- Severity assessment per finding
- Suggested test cases to prevent regression

**Workflow phase:** After `/review` and `/done-check`. High-value for Tier 2
complex features. Also useful before releases for critical paths.

**Model:** Opus

**Command spec (`validate/adversarial.md`):**

```markdown
# /adversarial — Hostile Environment and Complex Interaction Testing

## Instructions

Your goal is to find failure modes that a standard code review would miss.

Do NOT repeat findings that a basic code review covers. Assume that error
handling completeness, null/empty input edge cases, basic type safety, and
standard boundary conditions have already been verified by /review. If you
find those categories of issues, skip them — they belong in /review, not here.

Instead, focus on:
- Complex INTERACTIONS between components that create emergent failures
- HOSTILE ENVIRONMENTS where external systems misbehave
- TEMPORAL issues: race conditions, ordering dependencies, timing windows
- ADVERSARIAL INPUTS: not just "empty string" but intentionally crafted
  payloads designed to exploit assumptions

## Step 1: Understand Intent

Read the plan/spec to understand what the code SHOULD do.
Read the implementation to understand what it ACTUALLY does.
Identify assumptions the code makes about its environment and inputs.
Those assumptions are your attack surface.

## Step 2: Attack Vectors

Focus exclusively on these categories:

### State and Sequence Attacks
- Invalid state transitions (calling methods in unexpected order)
- Interleaved operations from multiple callers or threads
- Operation interrupted mid-execution (crash, timeout, signal, cancel)
- Retry after partial failure (is state consistent? are side effects idempotent?)
- State that drifts over time (counter overflow, cache staleness, connection pool exhaustion)

### Concurrency Attacks
- Race conditions between concurrent access paths
- Deadlock potential (lock ordering violations)
- Livelock under contention
- ABA problems in lock-free structures
- Stale reads after concurrent writes

### Environment Attacks
- External service timeout, connection reset, partial response
- Filesystem: permission denied, disk full, file locked by another process
- Network: DNS failure, TLS certificate expiry, proxy interference
- Resource exhaustion: file handle limit, thread pool saturation, memory pressure
- Clock: NTP jump, timezone change, daylight saving transition
- Platform: OS signal during critical section, OOM killer

### Adversarial Input Attacks
- Inputs crafted to trigger worst-case algorithmic complexity (e.g., hash collision
  floods, regex backtracking, deeply nested structures)
- Inputs that exploit string encoding assumptions (mixed encodings,
  overlong UTF-8 sequences, null bytes mid-string)
- Inputs that exceed implicit size assumptions not enforced by the code
- Serialized data from a previous version (schema evolution attacks)

### Cascade Failures
- One component failure propagating to unrelated components
- Error handling that itself causes errors (e.g., logging failure during error path)
- Retry storms amplifying a partial outage
- Circular dependencies revealed under failure conditions

## Step 3: Report

For each finding:

```markdown
## Finding <N>: <title>

**Severity:** critical / high / medium / low
**Vector:** <state / concurrency / environment / adversarial input / cascade>

**Reproduction:**
<exact steps, inputs, or conditions to trigger the failure>

**Expected behavior:**
<what should happen according to the spec>

**Actual behavior:**
<what actually happens — crash, wrong result, hang, data corruption, etc.>

**Root cause:**
<why the code fails — which assumption is violated>

**Suggested fix:**
<how to address it>

**Test case:**
<test code that would catch this regression>
```

Prioritize findings by: severity × likelihood of occurrence in production.
Findings that require rare but plausible conditions rank higher than findings
that require attacker access or extremely unusual environments.
```

---

#### `/fresh-eyes` — Implementation-Blind Code Assessment

**Trigger:** Want an assessment of code quality, clarity, and maintainability
from an agent that does not know the feature intent, plan, or implementation
decisions. Keywords: "fresh eyes", "new perspective", "what do you think of
this code", "pretend you're new", "readability check".

**Context required:**
- Code files to review
- CLAUDE.md will be auto-loaded (this is fine — see note below)
- NO plan file, NO scratchpad, NO implementation context

**Outcome:**
- What the agent thinks the code does (clarity test)
- What looks fragile or confusing
- What the agent would change as a new maintainer
- Comparison against user's intent reveals communication gaps in the code

**Workflow phase:** Optional but high-value step in Tier 2 after other validation.
Also useful periodically on mature codebases.

**Model:** Opus

**Command spec (`validate/fresh-eyes.md`):**

```markdown
# /fresh-eyes — Implementation-Blind Code Assessment

## Instructions

You are assessing code with no knowledge of the feature's intent, plan, or
implementation decisions. You may see CLAUDE.md content about the project's
general architecture and conventions — that's fine, you know the project exists.
But you do NOT know:
- What specific feature was just built
- What the plan or spec says
- What implementation choices were made or why
- What tradeoffs were considered

This is intentional. Your job is to assess the code purely on what it
communicates through its structure, naming, comments, and logic. If you
can't figure out what it does or why, that's a finding — the code has
a clarity problem that will affect future maintainers (human or AI).

## Step 1: Read the Code

Read the specified files. Do NOT read plan files, scratchpad, or any
external documentation. Only read what is in the source files themselves
(including inline comments and doc comments).

## Step 2: Assess

### What Does This Code Do?
Describe in your own words what you believe this code does.
Be specific. If parts are unclear, say so — "I can't determine what
this function is supposed to return when X happens" is a valid finding.

### What Looks Good?
- Clear abstractions or well-named interfaces
- Robust error handling
- Good separation of concerns
- Effective use of the type system
- Self-documenting code that doesn't need external context

### What Looks Fragile?
- Code that would break easily if assumptions change
- Hidden coupling between components
- Magic numbers or unexplained constants
- Complex control flow that's hard to follow
- Missing error handling on likely failure paths
- Implicit ordering dependencies

### What Would You Change?
If you were taking over maintenance of this code tomorrow:
- What would you refactor first?
- What would you want documented that isn't?
- What would you add tests for?
- What naming would you change for clarity?

## Step 3: Present

Format as a narrative, not a checklist. The user wants to understand
how the code reads to an outsider, not just a list of nitpicks.

End with: "My understanding of this code is: <one paragraph summary>.
Is that correct?" — if the user says no, the gap between intent and
expression is a documentation and naming problem worth addressing.
```

---

### TEST SKILLS

---

#### `/gen-tests` — Generate Tests

**Trigger:** Need tests written for new or existing code. Can run autonomously
in a separate worktree or subagent. Keywords: "write tests", "generate tests",
"add test coverage", "test this", "need tests for".

**Context required:**
- Target files or modules to test
- Plan file (for test approach section and done criteria)
- CLAUDE.md for test framework and conventions
- Existing test files (for style and fixture patterns)

**Outcome:**
- Test files written following language-specific conventions
- Tests cover happy path, edge cases, and error conditions
- Tests are committed separately from implementation code
- Test suite passes

**Workflow phase:** After implementation, before or alongside `/review`.
One of the best candidates for autonomous execution — can run in a worktree
or as a subagent while the developer does other work.

**Model:** Sonnet (pattern-following task)

**Command spec (`test/gen-tests.md`):**

```markdown
# /gen-tests — Generate Tests

## Instructions

You are generating tests for code that has already been implemented. Your
tests should verify the code works correctly, catch regressions, and
document expected behavior through examples.

Write tests that a future developer can read to understand what the code
is supposed to do — tests are documentation.

## Step 1: Identify Test Targets

If the user specified files or modules, test those. If not:
1. Read the plan's "Test Approach" section for guidance
2. Check `git diff main..HEAD --name-only` for recently changed files
3. Prioritize: new files first, then significantly modified files

## Step 2: Analyze Existing Test Patterns

Before writing any tests, examine existing test files in the project:
- What test framework is used?
- What naming conventions do tests follow?
- How are fixtures and test data set up?
- Where do test files live relative to source files?
- Are there shared test utilities or helpers?

Match the existing patterns exactly. Do not introduce a new testing style.

## Step 3: Write Tests (Language-Specific)

### Rust
- Place unit tests in `#[cfg(test)]` modules at the bottom of the source file
- Place integration tests in `tests/` directory
- Use `#[test]` for standard tests, `proptest!` for property-based tests
  if proptest is already a dependency
- Test error cases with `#[should_panic]` or by asserting `Result::Err`
- Add doc tests for public API examples where appropriate

### C++
- Place tests in the project's test directory (match existing structure)
- Use the project's test framework (Google Test, Catch2, etc.)
- One test file per source module
- Use descriptive test names: `TEST(ModuleName, DescribesExpectedBehavior)`
- Test both success and failure paths

### TypeScript / React
- Place tests adjacent to source: `Component.test.tsx` next to `Component.tsx`
- Use React Testing Library for component tests
- Use `describe` / `it` blocks with readable descriptions
- Test user-visible behavior, not implementation details
- Mock external dependencies (API calls, timers)

### Go
- Place tests in `_test.go` files in the same package
- Use table-driven tests for functions with multiple input/output cases
- Test exported functions; only test unexported if complex logic warrants it
- Use `t.Helper()` in test utility functions

## Step 4: Test Categories

For each test target, write tests in this priority order:

### Happy Path (mandatory)
- Each public function/method with typical valid inputs
- Expected output verified
- At least one test per public interface

### Edge Cases (mandatory)
- Empty/nil/zero inputs
- Single-element collections
- Boundary values
- Maximum/minimum valid inputs

### Error Conditions (mandatory for functions that can fail)
- Invalid inputs produce appropriate errors (not panics)
- Error messages are meaningful
- Error types are correct
- Partial failure doesn't corrupt state

### Integration (if multiple components interact)
- Data flows correctly between components
- Component A's output is valid input for component B
- Error in one component is handled by the caller

## Step 5: Validate

1. Run the full test suite: all new tests pass
2. Verify existing tests still pass (no regressions introduced)
3. Check that tests actually test something meaningful:
   - No tautological assertions (assert true == true)
   - No tests that pass regardless of implementation
   - Each test would FAIL if the behavior it tests were broken

## Step 6: Commit

```bash
git add <test files>
git commit -m "test: add tests for <module/feature>

Coverage:
- <N> unit tests
- <N> edge case tests
- <N> error condition tests
- <N> integration tests (if applicable)"
```

## CRITICAL RULES

- Do NOT modify implementation code — only create/modify test files
- Do NOT test private/internal implementation details unless they contain
  complex logic that warrants direct testing
- If a function is difficult to test in isolation, note it as a design
  smell in a comment but write the best test you can
- Match the project's existing test patterns exactly
```

---

### INTEGRATION SKILLS

---

#### `/predict-conflicts` — Pre-Merge Conflict Analysis

**Trigger:** About to merge parallel workstream branches and want to
identify conflicts before they happen. Keywords: "predict conflicts",
"merge analysis", "will these conflict", "pre-merge check".

**Context required:**
- List of branches to merge
- File lists from each branch (via git diff --name-only)
- Plan file for understanding intended merge order

**Outcome:**
- Per-file conflict risk assessment
- Recommended merge order
- Specific guidance for files that will conflict
- Pre-resolution decisions for predictable conflicts

**Workflow phase:** After all workstreams complete validation, before merging. Tier 2 only.

**Model:** Opus

**Command spec (`integrate/predict-conflicts.md`):**

```markdown
# /predict-conflicts — Pre-Merge Conflict Analysis

## Instructions

You are analyzing branches that are about to be merged. Your job is to
predict where conflicts will occur and prepare resolution strategies
BEFORE the merge, so the actual merge is fast and correct.

## Step 1: Gather Branch Data

For each branch to be merged:
```bash
git diff main..<branch> --name-only    # files changed
git diff main..<branch> --stat         # change volume per file
```

## Step 2: Identify Overlapping Files

Build a matrix: for each file, which branches modified it?

- **No overlap:** file modified by exactly one branch → clean merge
- **Read-only overlap:** file read by multiple but written by one → clean
- **Write overlap:** file modified by multiple branches → CONFLICT RISK

## Step 3: Analyze Conflicts

For each file with write overlap:

1. Examine the specific changes from each branch:
   ```bash
   git diff main..<branch-a> -- <file>
   git diff main..<branch-b> -- <file>
   ```

2. Classify the conflict:
   - **Non-overlapping regions:** both modified the file but different sections
     → git will likely auto-merge, but verify the combined result makes sense
   - **Same region, compatible:** both made similar changes (e.g., both added
     an import) → pick one or combine
   - **Same region, incompatible:** genuinely conflicting changes → needs
     human decision

3. For each conflict, recommend a resolution:
   - Which branch's version to keep, OR
   - How to manually reconcile, with specifics

## Step 4: Validate Merge Order

Check the plan's merge order against the dependency graph:
- Does any consumer branch get merged before its producer?
- Are shared interface files being introduced by the right branch?
- Will the test suite pass after each individual merge step?

## Step 5: Report

```markdown
## Merge Conflict Prediction

### Merge Order (validated)
1. <branch> — <rationale>
2. <branch> — <rationale>

### Clean Files (no overlap)
<count> files will merge without conflicts

### Auto-Mergeable (non-overlapping edits in same file)
- <file>: branch-a edits lines X-Y, branch-b edits lines Z-W → auto-merge safe

### Conflicts Requiring Resolution
| File | Branch A Change | Branch B Change | Recommended Resolution |
|------|----------------|----------------|----------------------|
| <file> | <what A did> | <what B did> | <recommendation> |

### Post-Merge Verification Steps
- [ ] Run full test suite after each merge step
- [ ] Verify <specific integration point> works across workstream boundary
```
```

---

#### `/integrate-review` — Post-Merge Integration Validation

**Trigger:** All workstreams merged, need to verify they work together
correctly. Keywords: "integration review", "post-merge check",
"do these work together", "cross-workstream validation".

**Context required:**
- The merged codebase
- Plan file (for understanding workstream boundaries)
- List of what each workstream implemented

**Outcome:**
- Integration issue report
- Interface mismatch detection
- Cross-workstream consistency validation

**Workflow phase:** After all merges complete, before release validation. Tier 2.

**Model:** Opus

**Command spec (`integrate/integrate-review.md`):**

```markdown
# /integrate-review — Post-Merge Integration Validation

## Instructions

Multiple workstreams have been merged. Your job is to find integration
issues — problems that exist BECAUSE separate agents built separate
parts, not bugs within a single workstream. Think: mismatched
assumptions, inconsistent patterns, missing glue code.

## Step 1: Load Context

Read the plan to understand:
- What each workstream was responsible for
- What interfaces were defined between them
- What the expected integration points are

## Step 2: Interface Verification

For each shared interface defined in the plan:
1. Find the interface definition
2. Find all implementations (from producer workstreams)
3. Find all usages (from consumer workstreams)
4. Verify: does usage match implementation?
   - Correct types passed?
   - Error cases handled?
   - Return values used correctly?
   - Null/optional/error states accounted for?

## Step 3: Cross-Workstream Consistency

Check for:
- **Naming inconsistency:** same concept named differently across workstreams
  (e.g., "user" vs "account" vs "player")
- **Error handling inconsistency:** one workstream uses Result, another throws
- **Logging inconsistency:** different log levels or formats for similar events
- **Config inconsistency:** same setting read from different env vars or files
- **Pattern inconsistency:** one workstream uses async, another blocks

## Step 4: Missing Integration

Look for:
- Frontend components that reference backend endpoints that don't exist
- Backend handlers that expect request formats the frontend doesn't send
- Database schemas that don't match the ORM models
- Event publishers without corresponding subscribers
- Config values referenced but never defined

## Step 5: Report

```markdown
## Integration Review: <feature name>

### Interface Compliance
| Interface | Status | Details |
|-----------|--------|---------|
| <name> | ✅ / ❌ | <specifics> |

### Integration Issues
<numbered list with file references and fix recommendations>

### Consistency Issues
<numbered list — lower priority but worth cleaning up>

### Missing Integration Points
<anything that needs glue code to connect workstreams>

### Verdict: <INTEGRATED / NEEDS WORK>
```
```

---

### RELEASE SKILLS

---

#### `/pre-release` — Pre-Release Audit

**Trigger:** Feature is validated and about to be released. Final checklist
before tagging. Keywords: "pre-release", "ready to release", "release check",
"final audit".

**Context required:**
- Changes since last release tag
- Full codebase access
- CLAUDE.md for project-specific release requirements

**Outcome:**
- Comprehensive checklist with pass/fail per item
- Blockers identified before release (not after)

**Workflow phase:** After all validation passes, before tagging. All tiers
for formal releases.

**Model:** Sonnet (checklist execution, not deep reasoning)

**Command spec (`release/pre-release.md`):**

```markdown
# /pre-release — Pre-Release Audit

## Instructions

You are running the final audit before a release tag. Be thorough. A bug
found here costs 10 minutes to fix. A bug found in production costs hours.

## Step 1: Identify Scope

```bash
# Find last release tag
LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")

# Files changed since last release
if [ -n "$LAST_TAG" ]; then
  git diff $LAST_TAG..HEAD --name-only
else
  echo "No previous tag found — auditing entire project"
fi
```

## Step 2: Audit Checklist

For all files changed since last release:

### Code Quality
- [ ] No TODO, FIXME, or HACK comments that should be resolved before release
- [ ] No debug/development code (print statements, console.log, test flags,
      hardcoded localhost URLs)
- [ ] No hardcoded values that should be configurable
- [ ] No commented-out code blocks (either remove or document why kept)

### Documentation
- [ ] All new public APIs have documentation comments
- [ ] README updated if user-facing behavior changed
- [ ] CHANGELOG updated with this release's changes
- [ ] Architecture docs reflect any structural changes
- [ ] API documentation (if applicable) is current

### Testing
- [ ] Full test suite passes: `<test command from CLAUDE.md>`
- [ ] New code paths have test coverage
- [ ] No skipped or disabled tests that should be re-enabled

### Version & Release
- [ ] Version bumped appropriately (semver)
- [ ] Breaking changes documented (if major version bump)
- [ ] Migration guide provided (if breaking changes)
- [ ] Dependencies updated and locked

### Security (if applicable)
- [ ] No secrets, tokens, or credentials in code
- [ ] No new dependencies with known vulnerabilities
- [ ] Input validation on all external-facing interfaces

## Step 3: Report

```markdown
## Pre-Release Audit

### Results
| Category | Pass | Fail | Manual |
|----------|------|------|--------|
| Code Quality | <n> | <n> | <n> |
| Documentation | <n> | <n> | <n> |
| Testing | <n> | <n> | <n> |
| Version | <n> | <n> | <n> |
| Security | <n> | <n> | <n> |

### Blockers (must fix before release)
<numbered list>

### Warnings (should fix, not blocking)
<numbered list>

### Verdict: <READY TO RELEASE / BLOCKED>
```
```

---

#### `/changelog` — Generate Changelog

**Trigger:** Need a changelog entry for a release. Keywords: "changelog",
"release notes", "what changed".

**Context required:**
- Git history since last release tag
- Commit messages and optionally PR descriptions
- Plan files for feature-level context

**Outcome:**
- Formatted changelog entry following Keep a Changelog conventions
- Grouped by: Added, Changed, Fixed, Removed, Security, Deprecated

**Workflow phase:** During release preparation, after pre-release passes.

**Model:** Sonnet

**Command spec (`release/changelog.md`):**

```markdown
# /changelog — Generate Changelog

## Instructions

Generate a changelog entry from git history. Follow the Keep a Changelog
format (https://keepachangelog.com/). Be concise — users care about what
changed and why, not implementation details.

## Step 1: Gather History

```bash
LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
if [ -n "$LAST_TAG" ]; then
  git log $LAST_TAG..HEAD --oneline --no-merges
else
  git log --oneline --no-merges
fi
```

Also check for plan files in `docs/plans/` (and `docs/plans/archive/`)
that correspond to this release for higher-level context on what features
were added.

## Step 2: Categorize

Group commits into:
- **Added:** new features or capabilities
- **Changed:** modifications to existing functionality
- **Fixed:** bug fixes
- **Removed:** removed features or deprecated items
- **Security:** security-related changes
- **Deprecated:** features that will be removed in a future release

Ignore:
- Merge commits
- WIP commits (these are intermediate, the final commit matters)
- Formatting-only changes
- Internal refactors with no user-visible impact (unless significant)

## Step 3: Write Entry

```markdown
## [<version>] - <YYYY-MM-DD>

### Added
- <user-visible description of what was added>

### Changed
- <user-visible description of what changed>

### Fixed
- <user-visible description of what was fixed>
```

Write descriptions from the USER's perspective, not the developer's.
"Added pathfinding for ground units" not "Implemented A* algorithm in
pathfinding_system.cpp with ground/aerial discriminator".

## Step 4: Update File

Prepend the new entry to CHANGELOG.md (create if it doesn't exist).
Preserve all existing entries.
```

---

#### `/catalog-debt` — Tech Debt Cataloging

**Trigger:** After a release or sprint, need to catalog accumulated tech debt
into an actionable backlog. Keywords: "catalog debt", "tech debt",
"what needs cleanup", "debt backlog", "post-sprint review".

**Context required:**
- Files changed since last release/tag
- Scratchpad files (for known issues and workarounds noted during build)
- Full codebase access for broader analysis

**Outcome:**
- Prioritized debt backlog as a markdown checklist
- Each item has blast radius, likelihood, and effort estimates
- Directly usable as a task list for the next development cycle

**Workflow phase:** After release. Also useful periodically for maintenance.

**Model:** Opus (needs judgment for prioritization)

**Command spec (`release/catalog-debt.md`):**

```markdown
# /catalog-debt — Tech Debt Cataloging

## Instructions

You are cataloging tech debt after a development sprint. Your job is to
produce a prioritized, actionable backlog — not a vague list of complaints.
Every item must be specific enough that an agent could implement the fix
from your description alone.

## Step 1: Gather Sources

1. **Changed files:** `git diff <last-tag>..HEAD --name-only`
2. **Scratchpads:** read all scratchpad*.md files for "Known Issues"
   and workarounds noted during the build
3. **TODO/FIXME scan:** `grep -rn "TODO\|FIXME\|HACK\|XXX" <src dirs>`
4. **Plan deviations:** check scratchpad "Decisions Made" sections for
   intentional shortcuts taken during the sprint

## Step 2: Analyze Each Source

For each file changed since last release:
- Is naming clear and consistent?
- Is error handling complete or were corners cut?
- Is there duplicated logic (especially if multiple workstreams added similar code)?
- Have module boundaries drifted from the documented architecture?
- Are there missing tests for new code paths?
- Are there performance concerns (allocations in loops, N+1 queries)?

For each TODO/FIXME/HACK:
- Is it still relevant?
- What's the fix?
- What's the risk of leaving it?

For each known issue from scratchpads:
- Is the workaround still in place?
- What's the proper fix?

## Step 3: Prioritize

Score each item on three dimensions:
- **Blast radius (B):** How much breaks if this causes a problem?
  - 3 = system-wide impact
  - 2 = module-level impact
  - 1 = isolated impact
- **Likelihood (L):** How likely is this to cause a problem?
  - 3 = will definitely cause issues soon
  - 2 = might cause issues under certain conditions
  - 1 = unlikely unless specific rare conditions
- **Effort (E):** How hard is the fix?
  - S = < 1 hour
  - M = 1-4 hours
  - L = 4+ hours

Priority score = B × L (effort helps with scheduling, not prioritization)

## Step 4: Output

Create or update `docs/debt-backlog.md`:

```markdown
# Tech Debt Backlog
Generated: <date>
Scope: changes since <last-tag>

## Critical Priority (score 6-9)
- [ ] **<title>** — <file:line or module>
  - Issue: <specific description>
  - Impact: <what goes wrong>
  - Fix: <specific recommendation>
  - Blast radius: <1-3> | Likelihood: <1-3> | Effort: <S/M/L>

## Medium Priority (score 3-5)
<same format>

## Low Priority (score 1-2)
<same format>

## Metrics
- Total items: <N>
- Critical: <N> | Medium: <N> | Low: <N>
- Quick wins (critical + small effort): <list>
```
```

---

### DOCUMENTATION SKILLS

---

#### `/docs` — Generate and Sync Documentation

**Trigger:** Need to add inline API documentation to new code AND/OR sync
architecture documentation with the current codebase. Keywords: "document",
"add docs", "update docs", "sync docs", "document API", "architecture docs".

**Context required:**
- Files containing new or changed public interfaces
- Existing documentation in `docs/` directory
- CLAUDE.md for documentation style conventions
- Recent changes (diff or plan file)

**Outcome:**
- Inline documentation comments added to all undocumented public interfaces
- Architecture docs updated to reflect current codebase structure
- Stale doc references removed, new modules documented
- All doc file paths verified to exist
- Single commit covering all documentation updates

**Workflow phase:** After implementation, before or after review. All tiers.
Also run after integration merge in Tier 2 for final doc sync.

**Model:** Sonnet

**Command spec (`docs/docs.md`):**

```markdown
# /docs — Generate and Sync Documentation

## Instructions

You are performing a comprehensive documentation pass. This covers two
complementary tasks: adding inline documentation to code, and syncing
external architecture documentation with the codebase. Both tasks ensure
that documentation matches the code as it exists right now.

## Part 1: Inline API Documentation

### Step 1.1: Identify Undocumented Interfaces

Scan the specified files (or recent diff if no files specified) for public
interfaces lacking documentation:
- **Rust:** pub fn, pub struct, pub enum, pub trait without /// comments
- **C++:** public class members, free functions in headers without /** */ or ///
- **Go:** exported functions/types without preceding comment
- **TypeScript:** exported functions/types/interfaces without JSDoc

### Step 1.2: Write Inline Documentation

For each undocumented interface, add documentation following the project's
existing style. Include:

**Functions/Methods:**
```
/// <One-line summary of what this does>
///
/// <Longer description if behavior is non-obvious>
///
/// # Arguments
/// * `<name>` - <what it is, valid range, constraints>
///
/// # Returns
/// <What is returned and under what conditions>
///
/// # Errors
/// <When this fails and what error types are returned>
///
/// # Examples
/// ```
/// <usage example for complex interfaces>
/// ```
```

**Types/Structs:**
```
/// <What this type represents>
///
/// <When to use this type, invariants it maintains>
```

**Traits/Interfaces:**
```
/// <What contract implementors must satisfy>
///
/// <Key methods and their relationship>
```

## Part 2: Architecture Documentation Sync

### Step 2.1: Inventory Documentation

Scan `docs/` and README files for architecture-related content:
- Architecture overviews
- Module descriptions
- Component diagrams (text-based)
- API specifications
- Setup/build instructions

### Step 2.2: Compare Against Code

For each documentation file:

1. Read the documented claims about project structure
2. Verify against actual codebase:
   - Do documented modules still exist?
   - Are module responsibilities accurately described?
   - Are documented file paths still correct?
   - Do documented interfaces match actual signatures?
   - Are build/test/run commands still accurate?

3. Identify:
   - **Stale content:** describes things that no longer exist or work differently
   - **Missing content:** new modules/components not yet documented
   - **Inaccurate content:** describes things incorrectly

### Step 2.3: Update

- Remove or correct stale content
- Add documentation for new modules/components
- Update file paths, command examples, and interface descriptions
- Preserve the documentation's existing style and level of detail
- Do NOT add excessive detail — match the granularity of existing docs

## Part 3: Verify and Commit

### Step 3.1: Verify

- All file paths mentioned in docs exist
- All commands mentioned in docs work
- All module names match actual directory/file names
- CLAUDE.md "Key Docs" section points to files that exist
- Inline documentation compiles (doc tests pass if applicable)
- No existing documentation was removed or degraded
- Style is consistent with the rest of the project

### Step 3.2: Commit

```bash
git add <all documentation changes>
git commit -m "docs: update inline API docs and sync architecture documentation

Inline docs added/updated for:
- <module 1>
- <module 2>

Architecture docs synced:
- <doc file>: <what changed>
"
```
```

---

#### `/doc-decision` — Record Architectural Decision

**Trigger:** An architectural decision was made during planning or implementation
that should be recorded for future reference. Keywords: "record decision",
"ADR", "why did we", "document architecture decision".

**Context required:**
- The decision being made
- Context and alternatives considered
- Rationale for the choice

**Outcome:**
- ADR (Architecture Decision Record) file in `docs/decisions/`
- Numbered and dated for chronological tracking
- Links to relevant plan files or code

**Workflow phase:** During planning (Tier 2) or whenever a significant
architectural choice is made. Cross-tier.

**Model:** Sonnet

**Command spec (`docs/doc-decision.md`):**

```markdown
# /doc-decision — Record Architectural Decision

## Instructions

You are recording an architectural decision using the ADR (Architecture
Decision Record) format. ADRs are immutable once written — if a decision
is reversed, a new ADR supersedes the old one rather than editing it.

## Step 1: Determine Next Number

```bash
ls docs/decisions/ 2>/dev/null | sort -n | tail -1
```

Increment the highest number. If no decisions directory exists, create it
and start at 001.

## Step 2: Write ADR

Create `docs/decisions/<NNN>-<short-title>.md`:

```markdown
# <NNN>. <Title>

**Date:** <YYYY-MM-DD>
**Status:** accepted
**Related plan:** <link to plan file if applicable>

## Context

<What is the situation that requires a decision? What forces are at play?
What constraints exist? Keep factual — this is the problem statement.>

## Decision

<What is the decision? State it clearly and directly.>

## Alternatives Considered

### <Alternative 1>
- Pros: <list>
- Cons: <list>
- Why rejected: <reason>

### <Alternative 2>
<same format>

## Consequences

### Positive
- <what this enables or improves>

### Negative
- <tradeoffs accepted, costs incurred>

### Neutral
- <side effects that are neither good nor bad>
```

## Step 3: Cross-Reference

- If a plan file drove this decision, add a reference in the plan
- If this supersedes a previous ADR, add "Supersedes: <NNN>" to the status
  and update the old ADR's status to "superseded by <this NNN>"
- Commit: "docs: ADR <NNN> - <short title>"
```

---

## Skill Interaction Map

This shows how skills chain together in common workflows:

```
STANDARD FEATURE (Tier 1):
  /plan → /plan-review (Sonnet) → /init → /implement →
  /gen-tests → /review → fix issues → /done-check →
  /docs → merge → /cleanup

COMPLEX FEATURE (Tier 2):
  /plan (includes parallelism analysis) → /plan-review (Opus) →
  /doc-decision → /build-interfaces →
    ┌─ /init ws-a → /implement → /gen-tests → /review → /done-check → /docs ─┐
    ├─ /init ws-b → /implement → /gen-tests → /review → /done-check → /docs ─┤
    └──────────────────────────────────────────────────────────────────────────┘
  /predict-conflicts → merge (ordered) → /integrate-review →
  /sweep (1-2 selected types) → /adversarial → /fresh-eyes (optional) →
  /docs (final sync) → /pre-release → /changelog →
  release → /catalog-debt → /cleanup

DEBT PAYDOWN CYCLE:
  /catalog-debt → prioritize → /plan (per debt item) →
  /init → /implement → /gen-tests → /review → /done-check → merge → /cleanup

SESSION RECOVERY:
  context full → /handoff → new session → /init → continue

TIER ESCALATION:
  working → scope grew → /handoff --escalate → new session → /plan → /init → continue
```
