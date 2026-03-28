# /pre-release — Pre-Release Audit

## Instructions

You are running the final audit before a release tag. Be thorough. A bug
found here costs 10 minutes to fix. A bug found in production costs hours.

## Step 1: Identify Scope

```bash
LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
if [ -n "$LAST_TAG" ]; then
  git diff $LAST_TAG..HEAD --name-only
else
  echo "No previous tag found — auditing entire project"
fi
```

## Step 2: Audit Checklist

### Code Quality
- [ ] No TODO, FIXME, or HACK comments that should be resolved before release
- [ ] No debug/development code (print statements, test flags, hardcoded localhost)
- [ ] No hardcoded values that should be configurable
- [ ] No commented-out code blocks

### Documentation
- [ ] All new public APIs have documentation comments
- [ ] README updated if user-facing behavior changed
- [ ] CHANGELOG updated with this release's changes
- [ ] Architecture docs reflect any structural changes

### Testing
- [ ] Full test suite passes
- [ ] New code paths have test coverage
- [ ] No skipped or disabled tests that should be re-enabled

### Version & Release
- [ ] Version bumped appropriately (semver)
- [ ] Breaking changes documented
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
