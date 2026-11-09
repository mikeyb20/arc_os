#!/usr/bin/env bash
# Build everything lane L needs on Linux (lane-l.md, M2-01):
#   1. each port's library and tools: its archived source is fetched (tools/fetch-archived.sh),
#      then ports/<name>/build.sh <prefix> builds it offline and installs into
#      build-lane/ports/<name>/ (the toolchain port, musl-cross-make, is installed by
#      tools/dev-setup.sh musl instead and skipped here);
#   2. the stack/ CMake project into build-lane/stack/.
# Needs the musl toolchain (tools/dev-setup.sh musl). Then: ctest --test-dir build-lane/stack -L lane
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build-lane"

# Each port's folder name is its entry in third_party/SOURCES.lock. Fetch them all first
# (SHA-256 checked, cached in third_party/cache), so the port builds themselves are offline.
ports=()
for script in "$ROOT"/ports/*/build.sh; do
    name="$(basename "$(dirname "$script")")"
    [[ "$name" == musl-cross-make ]] && continue
    ports+=("$name")
done
if ((${#ports[@]})); then
    "$ROOT/tools/fetch-archived.sh" "${ports[@]}"
fi
for name in "${ports[@]}"; do
    echo "[lane] port $name"
    "$ROOT/ports/$name/build.sh" "$OUT/ports/$name"
done

cmake -S "$ROOT/stack" -B "$OUT/stack" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain-musl.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/stack"
