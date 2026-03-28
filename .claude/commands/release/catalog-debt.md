# /catalog-debt — Tech Debt Cataloging

## Instructions

You are cataloging tech debt after a development sprint. Your job is to
produce a prioritized, actionable backlog — not a vague list of complaints.
Every item must be specific enough that an agent could implement the fix
from your description alone.

## Step 1: Gather Sources

1. **Changed files:** `git diff <last-tag>..HEAD --name-only`
2. **Scratchpads:** read all scratchpad*.md files for "Known Issues"
3. **TODO/FIXME scan:** grep for TODO, FIXME, HACK, XXX in source dirs
4. **Plan deviations:** check scratchpad "Decisions Made" sections

## Step 2: Analyze Each Source

For each changed file:
- Is naming clear and consistent?
- Is error handling complete?
- Is there duplicated logic?
- Have module boundaries drifted from documented architecture?
- Are there missing tests for new code paths?

For each TODO/FIXME/HACK:
- Is it still relevant? What's the fix? What's the risk of leaving it?

## Step 3: Prioritize

Score each item:
- **Blast radius (B):** 3=system-wide, 2=module-level, 1=isolated
- **Likelihood (L):** 3=will cause issues soon, 2=might under conditions, 1=unlikely
- **Effort (E):** S=<1hr, M=1-4hr, L=4+hr

Priority score = B x L

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
