#!/bin/sh
# lane.sqlite_shell (M2-02): the sqlite3 shell runs the M0-04 FTS5 script
# (tests/target/fts.sql, the same one the syscall inventory runs) and must print the one
# matching title. Driver: runs sqlite3 through $ARC_LANE_RUN (traced with ARC_LANE_LOGS).
#
#   sqlite_shell.sh <sqlite3> <fts.sql>
set -eu
sqlite3=$1
script=$2
dir=$(mktemp -d "${TMPDIR:-/tmp}/lane-sqlite.XXXXXX")
trap 'rm -rf "$dir"' EXIT
cp "$script" "$dir/fts.sql"
cd "$dir"
out=$("$ARC_LANE_RUN" sqlite_shell "$sqlite3" -bail -cmd ".read fts.sql" fts.db ".quit" < /dev/null)
if [ "$out" != "Hypothermia" ]; then
    echo "sqlite_shell: expected 'Hypothermia', got '$out'" >&2
    exit 1
fi
echo "sqlite_shell: ok"
