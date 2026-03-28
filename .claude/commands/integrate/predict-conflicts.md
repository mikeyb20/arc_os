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

1. Examine the specific changes from each branch
2. Classify the conflict:
   - **Non-overlapping regions:** different sections → likely auto-merge
   - **Same region, compatible:** similar changes → pick one or combine
   - **Same region, incompatible:** genuinely conflicting → needs decision
3. Recommend a resolution for each conflict

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
- <file>: branch-a edits lines X-Y, branch-b edits lines Z-W → safe

### Conflicts Requiring Resolution
| File | Branch A Change | Branch B Change | Recommended Resolution |
|------|----------------|----------------|----------------------|
| <file> | <what A did> | <what B did> | <recommendation> |

### Post-Merge Verification Steps
- [ ] Run full test suite after each merge step
- [ ] Verify <specific integration point> works across workstream boundary
```
