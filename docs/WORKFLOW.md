# Claude Code End-to-End Development Workflow v2

A practical, opinionated workflow for solo development across C++, Rust, Go, and React/TypeScript projects using Claude Code with parallel sessions.

This workflow is non-linear. Every phase can loop back to a previous phase when reality deviates from the plan. Escape hatches and recovery paths are documented throughout.

---

## How to Use This Document

**Pick a tier based on scope, then follow that tier's phases.** Cross-cutting concerns apply to all tiers.

| Tier | Scope | When to Use |
|------|-------|-------------|
| **Quick** | 1-2 files, clear fix | Bug fixes, config changes, small refactors, single-function additions |
| **Standard** | 3-10 files, single workstream | New features, module refactors, API additions, test suite buildouts |
| **Complex** | 10+ files or multiple workstreams | Cross-cutting features, major refactors, new subsystems, multi-layer changes |

**When in doubt, start one tier lower.** You can always escalate mid-task. Starting too high wastes time on ceremony; starting too low and escalating only costs a planning pause.

---

## Cross-Cutting Concerns

These apply regardless of tier. Set them up once per project.

### CC.1 CLAUDE.md Configuration

Keep under 60 lines. Use this skeleton:

```markdown
# [Project Name]

## Architecture
[One paragraph: what this project is, core patterns (ECS, clean arch, etc), key modules]

## Stack
- Language: [version]
- Build: [commands]
- Test: [commands]
- Lint: [commands]
- Run: [commands]

## Conventions
- [Naming: files, types, functions]
- [Error handling pattern: Result types, exceptions, error codes]
- [Module structure: where new code goes]

## Gotchas
- [Things Claude consistently gets wrong in this codebase]
- [Non-obvious constraints: "never use raw pointers in gameplay code"]
- [Dependencies that require special handling]

## Key Docs
- Architecture: see docs/architecture.md
- ECS patterns: see docs/ecs-guide.md
- API contracts: see docs/api-spec.md
```

**Rules for CLAUDE.md maintenance:**
- If Claude already does something correctly without being told, delete that line
- If Claude makes the same mistake twice, add a Gotcha for it
- Review and prune monthly — stale instructions are worse than no instructions
- Never embed full docs inline — use pointers to files

### CC.2 State Persistence (The Scratchpad)

Every project should have a `scratchpad.md` at the repo root (gitignored or in `docs/`). This is the handoff mechanism between sessions.

```markdown
# Scratchpad

## Current State
- Working on: [feature/task name]
- Branch: [branch name]
- Last completed step: [what was just finished]
- Next step: [what should happen next]

## Decisions Made During Implementation
- [Decision]: [rationale] (diverges from plan because [reason])
- [Decision]: [rationale]

## Open Questions
- [Question that needs resolving before continuing]

## Known Issues
- [Issue discovered during build, not yet addressed]
- [Workaround applied, needs proper fix]
```

**Session handoff protocol:**
1. Before ending any session: "Update scratchpad.md with current state, decisions made, and next steps"
2. Before starting any session: "Read scratchpad.md and docs/plans/[current-plan].md for context"

This eliminates the "re-explain everything" tax on fresh sessions. The scratchpad is the source of truth for where things stand *right now*, while the plan is the source of truth for where things *should go*.

### CC.3 Hook Configuration

Set up these hooks once per project. They automate the most common intervention points.

**PostToolUse — Auto-format after edits:**
```json
{
  "hooks": {
    "PostToolUse": [
      {
        "tool": "edit",
        "command": "your-formatter --file $TOOL_INPUT_PATH"
      }
    ]
  }
}
```
Caveat: formatting hooks can consume significant context tokens. If you notice context filling fast, switch to manual formatting between sessions instead.

**Stop — Run tests when Claude finishes a turn:**
```json
{
  "hooks": {
    "Stop": [
      {
        "command": "scripts/validate-turn.sh"
      }
    ]
  }
}
```

Where `validate-turn.sh` does:
```bash
#!/bin/bash
# Run build
if ! make build 2>/dev/null; then
  echo "BUILD FAILED — fix before continuing"
  exit 1
fi
# Run tests
if ! make test 2>/dev/null; then
  echo "TESTS FAILED — fix before continuing"
  exit 1
fi
echo "Build and tests passing."
```

**PreToolUse — Scope enforcement for parallel work:**
```json
{
  "hooks": {
    "PreToolUse": [
      {
        "tool": "edit",
        "command": "scripts/check-scope.sh $TOOL_INPUT_PATH"
      }
    ]
  }
}
```

Where `check-scope.sh` reads an `ALLOWED_FILES` list and rejects edits outside scope. This turns the "do NOT modify files outside this scope" instruction from a suggestion into a hard gate.

### CC.4 Model Selection

| Task | Recommended Model | Rationale |
|------|------------------|-----------|
| Planning, architecture | Opus | Deeper reasoning, better at identifying edge cases and tradeoffs |
| Standard implementation | Sonnet | Fast, cost-effective, good at following established patterns |
| Complex implementation | Opus | Better at multi-file coordination and novel problem-solving |
| Test generation | Sonnet | Pattern-following task, doesn't need deep reasoning |
| Validation / code review | Opus | Catches subtle bugs that Sonnet misses |
| Adversarial testing | Opus | Requires creative thinking about failure modes |
| Documentation | Sonnet | Straightforward generation task |
| Formatting / linting | Sonnet | Mechanical task |

**Rate limit strategy:**
- Plan your day around the 5-hour reset window
- Front-load Opus work (planning, complex implementation) early in the window
- Use Sonnet for routine tasks to preserve Opus budget
- If you hit limits mid-feature: switch to Sonnet, shift to documentation/test tasks, or take a code review break
- Use `/model` to switch within a session, or set the default per-session

### CC.5 Subagents vs. Worktrees

These solve different problems. Don't use worktrees when subagents suffice.

**Subagents** — lightweight parallel tasks within a single session:
- Codebase exploration (4 agents scanning different directories)
- Parallel research (architecture patterns, library comparison, security audit)
- Read-only analysis (dependency graphs, complexity metrics, dead code detection)
- No filesystem isolation needed — they share the main session's working directory
- Each gets its own context window
- Parallelism capped at ~10 concurrent
- Use via: "Explore the codebase using 4 tasks in parallel, each covering a different module"

**Worktrees** — full filesystem isolation for write-heavy parallel work:
- Parallel feature implementation
- Separate workstreams that modify different files
- Long-running autonomous tasks alongside interactive work
- Each gets its own branch, working directory, and Claude session
- Use via: `claude -w workstream-name`

**Rule of thumb:** If the parallel tasks only *read* code, use subagents. If they *write* code, use worktrees.

### CC.6 Test Strategy

Testing approach varies by language and project type:

**C++ (Arcane Industry type projects):**
- Unit tests for pure logic (algorithms, data transformations, ECS system behavior)
- Integration tests for system interactions (pathfinding + collision, resource loading + rendering)
- Snapshot tests for serialization/deserialization
- Manual playtesting for gameplay feel (agent can't validate this)
- Framework: Google Test, Catch2, or project-specific
- When to write: After implementation, against the agreed interface. TDD is impractical for game systems where the interface evolves during implementation.

**Rust (ForgeHub type projects):**
- Unit tests inline with `#[cfg(test)]` modules
- Integration tests in `tests/` directory for CLI behavior and cross-module flows
- Property-based tests (proptest) for parsers, serializers, and data transformations
- Doc tests for public API examples
- When to write: TDD works well for Rust CLIs — define the interface, write tests against it, then implement. The type system catches so much that tests focus on logic and edge cases.

**React/TypeScript (WifiSense frontend type):**
- Component tests with React Testing Library
- Hook tests for custom hooks with complex state
- Integration tests for data flow (API call → state → render)
- E2E tests (Playwright) for critical user paths
- When to write: Component tests during implementation. E2E tests after integration.

**General test prompting patterns:**
```
# After implementation — generate tests
Write tests for [module]. Focus on:
1. Happy path for each public function
2. Edge cases: empty inputs, boundary values, error conditions
3. Any invariants that must hold (e.g., "balance never goes negative")
Do NOT test private implementation details.

# Test quality check — is the code testable?
Try to write unit tests for [module]. If you struggle to isolate
any function for testing, flag it — that's a design smell.
```

---

## Tier 0: Quick

**Scope:** 1-2 files, clear problem, obvious fix.

### Workflow

```
1. Open Claude Code (or use existing session)
2. Describe the fix: "Fix [bug/issue] in [file]. The problem is [X], it should [Y]."
3. Let Claude implement
4. Review the diff
5. Run tests (or let Stop hook handle it)
6. Commit
```

No plan file. No separate validation session. No worktrees. The overhead isn't justified.

**Escalation trigger:** If during implementation you realize the change touches more files than expected, or requires design decisions, stop and escalate to Tier 1.

```
# Escalation prompt
Stop. This is bigger than I thought. Let's switch to Plan Mode and
scope this properly before continuing.
```

---

## Tier 1: Standard

**Scope:** 3-10 files, single workstream, one feature or refactor.

### Phase 1S: Lightweight Plan

Create `docs/plans/[feature-name].md`:

```markdown
# [Feature Name]

## What
[One sentence]

## Why
[Problem this solves]

## Boundaries
[What this does NOT touch]

## Files to Create/Modify
- [path]: [what changes]
- [path]: [what changes]

## Interfaces
- [function/type signatures this introduces or changes]

## Done Criteria
- [ ] [Specific, testable condition]
- [ ] [Specific, testable condition]
- [ ] All existing tests pass
- [ ] New tests cover [specific scenarios]

## Test Approach
- [What to test, what framework, TDD or post-implementation]
```

Open Plan Mode. Share the plan:

```
Review this plan. Identify:
1. Decisions I haven't made that you'd need to guess at
2. Files I've missed that will need changes
3. Risks or edge cases I haven't considered
```

Iterate until clean, then approve. **Budget: 10-15 minutes.** If planning takes longer than this, you probably need Tier 2.

### Phase 2S: Implementation with Inline Validation

Initialize the session:

```
Read the plan at docs/plans/[feature].md.
Implement it following this loop for each task:
1. Implement the change
2. Run build + tests
3. Before committing, do a quick self-review: check for edge cases,
   missing error handling, and consistency with the plan
4. Commit with a descriptive message
5. Update scratchpad.md with progress

If you discover the plan is wrong or incomplete, STOP and tell me
what needs to change before continuing.
```

The "self-review before commit" step is the key addition — it catches issues during the build loop rather than deferring everything to post-build validation.

**Plan deviation handling:** When Claude (or you) discovers the plan doesn't account for something:

```
# If the deviation is small (doesn't change architecture):
Note the deviation in scratchpad.md under "Decisions Made During Implementation"
and continue.

# If the deviation is significant (changes interfaces, scope, or approach):
STOP. Update the plan file. If parallel workstreams exist, assess whether
the change affects their scope or interfaces. Resume only after the plan
is consistent again.
```

### Phase 3S: Post-Build Validation

**Done criteria check (mandatory):**

```
Compare the implementation against the done criteria in docs/plans/[feature].md.
For each criterion, verify:
- Is it met? (yes/no with evidence)
- If not, what's missing?
```

**Fresh-session review (recommended for anything non-trivial):**

```bash
claude  # fresh session, no prior context
```

```
Review the diff between main and [feature-branch].
Focus on correctness and robustness only:
1. Edge cases that could cause failures
2. Error handling gaps
3. Spec compliance (see docs/plans/[feature].md)
Do NOT suggest style changes.
```

### Phase 4S: Merge and Close

```bash
git checkout main
git merge feature/[feature-name]
# Run full test suite
# If tests pass, delete branch and plan file (or archive it)
```

Update scratchpad.md to reflect completion.

---

## Tier 2: Complex

**Scope:** 10+ files, multiple workstreams, cross-cutting changes.

### Phase 1C: Full Planning

Create `docs/plans/[feature-name].md` with the full template:

```markdown
# [Feature Name]

## What
[One paragraph describing the deliverable]

## Why
[Problem this solves, context for the decision]

## Boundaries
[What this does NOT touch — be explicit]

## Architecture Impact
[How this changes the system's structure, if at all]
[New dependencies introduced]
[Modules affected]

## Workstream Breakdown

### Shared Interfaces (must be built first)
- [type/interface]: [file path] — [description]
- [type/interface]: [file path] — [description]

### Workstream A: [Name]
- **Files:** [exact list]
- **Depends on:** [interfaces from shared or other workstreams]
- **Exposes:** [interfaces other workstreams consume]
- **Estimated complexity:** [low/medium/high]

### Workstream B: [Name]
- **Files:** [exact list]
- **Depends on:** [interfaces from shared or other workstreams]
- **Exposes:** [interfaces other workstreams consume]
- **Estimated complexity:** [low/medium/high]

## Conflict Risk Assessment
- [File/module] is touched by workstreams [A] and [B] — resolution: [who owns it, or build sequentially]
- [Shared config/build files] — resolution: [one workstream owns, others don't touch]

## Done Criteria
- [ ] [Specific, testable condition]
- [ ] [Specific, testable condition]
- [ ] All workstream interfaces match their contracts
- [ ] Integration tests pass across workstream boundaries
- [ ] All existing tests pass

## Test Approach
[Per-workstream test strategy]
[Integration test strategy for cross-workstream boundaries]

## Merge Order
1. Shared interfaces
2. [Producer workstream]
3. [Consumer workstream]
4. [Test/docs workstream]
```

Plan Mode session (use **Opus** for this):

```
Review this plan. I need you to:
1. Identify decisions I haven't made
2. Validate the workstream breakdown — are file assignments clean or will they conflict?
3. Check the merge order for dependency correctness
4. Propose integration test cases that span workstream boundaries
5. Identify the riskiest part of this plan
```

**Budget: 20-40 minutes.** This is the highest-leverage time in the entire workflow.

### Phase 2C: Contract-First Interface Build

Before any parallel work begins, build shared interfaces in a dedicated session:

```bash
claude  # dedicated session for interfaces only
```

```
Implement ONLY the shared interfaces from docs/plans/[feature].md:
- [list of types, traits, function signatures, API contracts]
Include type definitions, trait declarations, and function signatures.
Implement stub/placeholder bodies where needed.
Do NOT implement business logic.
Commit when done.
```

After committing, this becomes the base commit that all parallel workstreams branch from.

```bash
git checkout -b feature/[feature]-interfaces
# ... build interfaces ...
git commit -m "feat: add shared interfaces for [feature]"
git checkout main
git merge feature/[feature]-interfaces
```

### Phase 3C: Parallel Implementation

Spin up worktrees from the post-interface commit:

```bash
claude -w workstream-a    # Terminal 1
claude -w workstream-b    # Terminal 2
```

Initialize each session with:

```
Read the plan at docs/plans/[feature].md. You are implementing Workstream [A].

YOUR SCOPE:
- Files to modify: [exact list]
- Files that are READ-ONLY (shared interfaces): [exact list]
- Files that are OFF-LIMITS (other workstream): [exact list]

YOUR INTERFACES:
- You depend on: [list what you consume]
- You expose: [list what you produce]

WORKFLOW:
1. Implement each task from the plan
2. Build + test after each change
3. Self-review before each commit: check edge cases, error handling, plan compliance
4. Commit with descriptive messages after each subtask
5. Update scratchpad.md with progress and any deviations

If you discover the plan needs to change or your workstream needs
files outside your scope, STOP and tell me. Do not improvise.
```

**Set up scope-enforcement hooks** (see CC.3) so the "off-limits" constraint is enforced mechanically, not just by instruction.

**Monitoring parallel sessions:**
- Check each session every 15-20 minutes
- Read scratchpad.md updates for deviation signals
- If one workstream discovers a plan problem, pause ALL workstreams until resolved

**Plan deviation in parallel context:**
```
# If the deviation affects only this workstream:
Note in scratchpad.md, continue.

# If the deviation affects shared interfaces or other workstreams:
STOP ALL WORKSTREAMS.
Update the plan.
Assess whether other workstreams need to rebase or adjust.
Resume only when all workstreams have consistent plans.
```

This is the most important recovery path in the entire workflow. A workstream silently diverging from the agreed interfaces is the #1 cause of integration pain.

### Phase 4C: Layered Validation

Run these as separate sessions. Use **Opus** for validation.

**4C.1 — Per-workstream validation (before merging):**

For each workstream, in a fresh session:

```
Review the diff for workstream [A] (branch: workstream-a).
Check against the plan at docs/plans/[feature].md.
1. Does it implement everything assigned to Workstream A?
2. Does it respect the shared interfaces without modifying them?
3. Are there edge cases, panics, or undefined behavior?
4. Is error handling complete?
5. Does it meet its done criteria?
```

**4C.2 — Done criteria verification (mandatory):**

```
Review docs/plans/[feature].md done criteria.
For each criterion, check the combined implementation across all workstreams.
Report: met / not met / partially met, with evidence.
```

**4C.3 — Focused sweeps (as needed, based on change type):**

| Sweep | When to Run | Prompt Focus |
|-------|-------------|-------------|
| Safety | Always for C++/Rust | UB, unwrap without justification, missing error handling |
| Architecture | After cross-cutting changes | Module boundaries, dependency direction, abstraction leaks |
| Performance | For hot paths, game loops | Allocations per frame, cache patterns, unnecessary copies |
| Duplication | After multi-workstream merges | Near-duplicate logic introduced by separate workstreams |

**4C.4 — Adversarial testing:**

```
Given the implementation of [feature], find inputs, states, or sequences that cause:
- Panics, crashes, or undefined behavior
- Incorrect results or silent data corruption
- Deadlocks, livelocks, or resource leaks
- Behavior that contradicts the spec at docs/plans/[feature].md

For each finding, provide a concrete reproduction case and severity assessment.
```

**4C.5 — Fresh eyes (recommended for complex features):**

```
You've never seen this codebase. Read [list of key files for this feature].
1. What does this code do? (If your explanation doesn't match my intent, I have a clarity problem)
2. What looks fragile or likely to break under change?
3. What would you refactor if you were taking over maintenance?
```

### Phase 5C: Integration

**5C.1 — Pre-merge conflict prediction:**

Before merging anything, in a fresh session:

```
I'm about to merge these branches into main in this order:
1. workstream-a (files: [list])
2. workstream-b (files: [list])

Analyze the file lists for potential conflicts:
- Files modified by multiple workstreams
- Shared config files that both may have modified
- Import/include changes that could conflict
- Any shared state or globals that both workstreams touch

For each potential conflict, recommend: which workstream's version wins,
or how to manually reconcile.
```

**5C.2 — Ordered merge:**

```bash
git checkout main

# 1. Merge producer workstreams first
git merge workstream-a
make test  # FULL suite, not just workstream-a tests

# 2. Merge consumer workstreams
git merge workstream-b
make test  # FULL suite

# 3. If conflicts occur, resolve with explicit intent (see Error Recovery)
```

**5C.3 — Post-merge integration validation:**

```
The following workstreams were just merged into main:
- workstream-a: [what it did]
- workstream-b: [what it did]

Review the combined codebase for:
1. Interface mismatches between workstreams
2. Inconsistent error handling across boundaries
3. Assumptions one workstream made about another that don't hold
4. Missing integration points (frontend calls backend endpoint that doesn't exist, etc.)
5. Duplicate or conflicting implementations of similar logic
```

### Phase 6C: Release

**6C.1 — Pre-release checklist:**

```
Run a pre-release audit on changes since [last-release-tag]:
1. TODO, FIXME, or HACK comments in changed files
2. Public APIs without documentation
3. Hardcoded values that should be configurable
4. Test coverage gaps for new code paths
5. CHANGELOG updated
6. Version bumped appropriately (semver)
7. Any debug/development code left in (print statements, test flags)
```

**6C.2 — Release automation (via `claude -p`):**

```bash
# Changelog from commits
claude -p "Generate a changelog from commits since $(git describe --tags --abbrev=0). Group by: Added, Changed, Fixed, Removed."

# Version bump
claude -p "Bump version to [X.Y.Z] in [Cargo.toml/package.json/CMakeLists.txt]"

# Tag and push
git tag -a v[X.Y.Z] -m "Release v[X.Y.Z]"
git push origin main --tags
```

**6C.3 — Post-release debt cataloging:**

```
Walk through every file modified since [last-release-tag].
For each, identify:
- Unclear naming or abstractions
- Missing error handling
- Duplicated logic
- Module boundary drift
- Missing tests
- TODO/FIXME items
- Workarounds noted in scratchpad.md

Output a prioritized backlog ranked by:
1. Blast radius (how much breaks if this fails)
2. Likelihood of causing issues in the next development cycle
3. Effort to fix (S/M/L)

Format as a markdown checklist I can use directly as a task list.
```

---

## Error Recovery Playbook

### Context window exhausted mid-feature

```
1. Have Claude update scratchpad.md: "Write current state, all decisions made,
   and exact next steps to scratchpad.md"
2. Commit all current work
3. /clear (or start new session)
4. "Read scratchpad.md and docs/plans/[feature].md. Continue from where
   the previous session left off."
```

### Plan discovered to be wrong mid-implementation

```
1. Stop implementation immediately
2. Commit current work (even if incomplete) with message "WIP: pausing for plan revision"
3. Switch to Plan Mode
4. Update the plan file with what you've learned
5. If parallel workstreams exist:
   a. Check if the change affects their scope or interfaces
   b. If yes: pause all workstreams, update plan, communicate changes
   c. If no: note the change in scratchpad.md, continue
6. Resume implementation from the updated plan
```

### Parallel workstream produced wrong interface

```
1. Stop the consuming workstream
2. Identify the mismatch: what was expected vs. what was produced
3. Decide: does the producer or consumer need to change?
4. Fix the source of truth (plan file)
5. Fix the producing workstream first, commit
6. Rebase consuming workstream onto the fix
7. Continue
```

### Tests pass but feature doesn't work

```
1. This means your tests don't cover the actual behavior — it's a test gap
2. Write a failing test that reproduces the actual problem first
3. Then fix the implementation until that test passes
4. Add the scenario to your test strategy for future features
```

### Integration reveals fundamental design flaw

```
1. Don't try to patch it — this is the most expensive mistake in the playbook
2. Revert to pre-merge main
3. Return to Phase 1 planning with what you've learned
4. Revise the architecture
5. Assess how much of the existing implementation is salvageable
6. Re-plan workstreams against the new architecture
7. Re-implement (existing code in branches can be cherry-picked where applicable)
```

### Two failed corrections in a session

```
1. Don't try a third correction — context is polluted
2. Note what went wrong and what you learned in scratchpad.md
3. /clear
4. Write a better prompt that incorporates the failure lessons upfront
5. If the same approach fails again, the problem is likely in the plan, not the prompt
```

### Rate limit hit mid-work

```
1. Commit all current work
2. Update scratchpad.md with state
3. Options:
   a. Switch to Sonnet for remaining implementation work
   b. Shift to tasks that don't need Opus (docs, tests, formatting)
   c. Do manual code review while waiting for reset
   d. Work on a different project until reset
4. When limits reset, resume from scratchpad state
```

---

## Quick Reference

### Session Types

| Session Type | Model | Context | Permissions | Duration |
|-------------|-------|---------|-------------|----------|
| Planning | Opus | Plan Mode, full repo | Read-only | 10-40 min (scales with tier) |
| Interface Build | Opus | Scoped to interfaces | Full | 15-30 min |
| Implementation | Sonnet (standard) / Opus (complex) | Scoped to workstream | Full or skip-permissions | 30-120 min |
| Inline Validation | Same as implementation | Same session | Same | Continuous |
| Post-Build Review | Opus | Fresh, diff-only | Read-only | 10-20 min |
| Adversarial | Opus | Fresh, scoped | Read-only | 10-20 min |
| Integration | Opus | Post-merge, full repo | Full | 15-30 min |
| Debt Audit | Sonnet or Opus | Fresh, full repo | Read-only | 20-40 min |

### Tier Decision Tree

```
How many files does this change touch?

  1-2 files, obvious fix?
    → Tier 0: Quick. Just do it.

  3-10 files, single concern?
    → Tier 1: Standard. Lightweight plan, single workstream.

  10+ files, OR multiple concerns, OR cross-cutting?
    → Tier 2: Complex. Full plan, parallel workstreams.

  Not sure yet?
    → Start at Tier 0. Escalate when complexity reveals itself.
```

### Anti-Patterns

- **Kitchen-sink session** — mixing unrelated tasks in one session. `/clear` between topics.
- **Correcting in circles** — after two failed corrections, `/clear` and rewrite the prompt.
- **Over-specified CLAUDE.md** — if Claude does it right without instruction, delete the line.
- **Agent-owned architecture** — you make design decisions, the agent implements them.
- **Validating in the build session** — fresh session for review, always.
- **Over-parallelizing** — 2-3 concurrent workstreams max for solo dev. You are the bottleneck.
- **Silent plan deviation** — any deviation from the plan must be recorded. Unrecorded deviations compound into integration nightmares.
- **Skipping done criteria** — every feature has a "how do I know this is finished" check. No exceptions.
- **Using worktrees for read-only tasks** — use subagents for parallel analysis, worktrees for parallel writes.
