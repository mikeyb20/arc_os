#!/bin/bash
# check-scope.sh — PreToolUse hook: reject edits outside allowed scope
# Usage: check-scope.sh <file-path>
#
# Reads .claude/scope.txt for a list of allowed file patterns (one per line).
# If the file exists and the edited path doesn't match any pattern, exit 1.

SCOPE_FILE=".claude/scope.txt"
TARGET="$1"

# If no scope file, allow everything
if [ ! -f "$SCOPE_FILE" ]; then
    exit 0
fi

# Check each pattern
while IFS= read -r pattern; do
    # Skip empty lines and comments
    [[ -z "$pattern" || "$pattern" =~ ^# ]] && continue

    # Use bash pattern matching
    if [[ "$TARGET" == $pattern ]]; then
        exit 0
    fi
done < "$SCOPE_FILE"

echo "SCOPE VIOLATION: $TARGET is not in the allowed file list (.claude/scope.txt)"
exit 1
