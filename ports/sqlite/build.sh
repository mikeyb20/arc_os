#!/usr/bin/env bash
# SQLite 3.54.1 (amalgamation) for arc_os (M2-02): libsqlite3.a, sqlite3.h and the
# sqlite3 shell, static, FTS5 built in, built with the pinned musl toolchain. Offline;
# each compile option's reason is in PORT.md.
#
#   ports/sqlite/build.sh <prefix>      (tools/lane/build.sh: build-lane/ports/sqlite)
set -euo pipefail
. "$(cd "$(dirname "$0")/../.." && pwd)/tools/lane/port-env.sh"
port_begin sqlite "${1:?usage: $0 <prefix>}"

# The same options for the library and the shell: the shell's commands depend on them
# (with SQLITE_OMIT_LOAD_EXTENSION it has no .load).
OPTS=(-DSQLITE_ENABLE_FTS5 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1
      -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_USE_URI=1)
CFLAGS=(-O2 "$ARC_PORT_ARCH" "${OPTS[@]}")

cd "$WORK"
"$CC" "${CFLAGS[@]}" -c sqlite3.c -o sqlite3.o
"$AR" rcs libsqlite3.a sqlite3.o
"$CC" "${CFLAGS[@]}" -static -o sqlite3 shell.c libsqlite3.a

cp libsqlite3.a "$PREFIX/lib/"
cp sqlite3.h "$PREFIX/include/"
cp sqlite3 "$PREFIX/bin/"
"$STRIP" "$PREFIX/bin/sqlite3"
port_check_static "$PREFIX/bin/sqlite3"
echo "[sqlite] installed into $PREFIX"
