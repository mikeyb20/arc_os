#!/usr/bin/env bash
# Build everything lane L needs on Linux (lane-l.md, M2-01):
#   1. each port's library and tools: ports/<name>/build.sh <prefix> installs into
#      build-lane/ports/<name>/ (the toolchain port, musl-cross-make, is installed by
#      tools/dev-setup.sh musl instead and skipped here);
#   2. the stack/ CMake project into build-lane/stack/.
# Needs the musl toolchain (tools/dev-setup.sh musl). Then: ctest --test-dir build-lane/stack -L lane
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build-lane"

for script in "$ROOT"/ports/*/build.sh; do
    name="$(basename "$(dirname "$script")")"
    [[ "$name" == musl-cross-make ]] && continue
    echo "[lane] port $name"
    "$script" "$OUT/ports/$name"
done

cmake -S "$ROOT/stack" -B "$OUT/stack" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain-musl.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/stack"
