# /changelog — Generate Changelog

## Instructions

Generate a changelog entry from git history. Follow the Keep a Changelog
format. Be concise — users care about what changed and why, not
implementation details.

## Step 1: Gather History

```bash
LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
if [ -n "$LAST_TAG" ]; then
  git log $LAST_TAG..HEAD --oneline --no-merges
else
  git log --oneline --no-merges
fi
```

Also check `docs/plans/` (and `docs/plans/archive/`) for plan files
corresponding to this release for higher-level feature context.

## Step 2: Categorize

Group commits into:
- **Added:** new features or capabilities
- **Changed:** modifications to existing functionality
- **Fixed:** bug fixes
- **Removed:** removed features or deprecated items
- **Security:** security-related changes
- **Deprecated:** features that will be removed in a future release

Ignore: merge commits, WIP commits, formatting-only changes.

## Step 3: Write Entry

```markdown
## [<version>] - <YYYY-MM-DD>

### Added
- <user-visible description>

### Changed
- <user-visible description>

### Fixed
- <user-visible description>
```

Write from the USER's perspective, not the developer's.

## Step 4: Update File

Prepend the new entry to CHANGELOG.md (create if it doesn't exist).
Preserve all existing entries.
