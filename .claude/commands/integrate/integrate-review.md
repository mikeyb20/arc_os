# /integrate-review — Post-Merge Integration Validation

## Instructions

Multiple workstreams have been merged. Your job is to find integration
issues — problems that exist BECAUSE separate agents built separate
parts, not bugs within a single workstream.

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

## Step 3: Cross-Workstream Consistency

Check for:
- **Naming inconsistency:** same concept named differently across workstreams
- **Error handling inconsistency:** different patterns for similar operations
- **Logging inconsistency:** different log levels or formats
- **Pattern inconsistency:** different approaches to the same problem

## Step 4: Missing Integration

Look for:
- Interfaces referenced but not implemented
- Event publishers without corresponding subscribers
- Config values referenced but never defined
- Missing glue code to connect workstreams

## Step 5: Report

```markdown
## Integration Review: <feature name>

### Interface Compliance
| Interface | Status | Details |
|-----------|--------|---------|
| <name> | PASS/FAIL | <specifics> |

### Integration Issues
<numbered list with file references and fix recommendations>

### Consistency Issues
<numbered list — lower priority but worth cleaning up>

### Missing Integration Points
<anything that needs glue code>

### Verdict: <INTEGRATED / NEEDS WORK>
```
