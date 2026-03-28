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

## Step 3: Validate Against Codebase

Explore the actual codebase to verify:
- Do the files listed actually exist (for modifications)?
- Are there files the plan missed that will need changes?
- Do the interfaces match existing patterns in the codebase?
- Are the named modules/types/functions spelled correctly?

## Step 4: Identify Risks

Look for:
- Decisions the plan defers to implementation (these become ambiguity)
- Implicit assumptions that aren't stated
- Cross-cutting concerns not addressed
- Migration or backward-compatibility issues

## Step 5: Report

### Blocking Issues (must fix before implementation)
<numbered list with specific recommendation for each>

### Should Fix (will cause pain later if ignored)
<numbered list with specific recommendation for each>

### Suggestions (improvements, not required)
<numbered list>

### Verdict
<READY FOR IMPLEMENTATION / NEEDS REVISION — with summary of what to fix>
