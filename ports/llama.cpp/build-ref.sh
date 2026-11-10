#!/usr/bin/env bash
# Test-only reference build of llama.cpp (M2-03): the same source and options.cmake, built
# with the host's GCC 14 (glibc, dynamically linked; tools/dev-setup.sh apt installs g++-14)
# and the same CPU floor (-march=x86-64-v3). It never ships; the lane tests compare its output
# with the musl port's, which answers "did musl or our toolchain change the math?". GCC 14 on
# purpose: the musl toolchain is GCC 14.1, and Ubuntu's default GCC 13 generates different
# float code (embeddings differed at cosine 0.99988; with GCC 14 they are bit-identical).
# The source is upstream's, without ports/llama.cpp/patches (0001 only swaps an #include).
#
#   ports/llama.cpp/build-ref.sh <dir>      (tools/lane/build.sh: build-lane/ref/llama.cpp)
#   tools: <dir>/bin/llama-completion, llama-embedding, llama-tokenize
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:?usage: $0 <dir>}"
SRC="$ROOT/third_party/src/llama.cpp"
[[ -d "$SRC" ]] || { echo "[llama.cpp-ref] no source in $SRC (tools/fetch-archived.sh llama.cpp)" >&2; exit 1; }
command -v g++-14 >/dev/null || { echo "[llama.cpp-ref] g++-14 missing (tools/dev-setup.sh apt)" >&2; exit 1; }

rm -rf "$OUT"
# Out-of-tree build: CMake writes nothing into the source directory.
cmake -S "$SRC" -B "$OUT/build" -G Ninja -C "$HERE/options.cmake" \
    -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
    -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3
cmake --build "$OUT/build" -j"$(nproc)" --target llama-completion llama-embedding llama-tokenize
mkdir -p "$OUT/bin"
cp "$OUT"/build/bin/llama-completion "$OUT"/build/bin/llama-embedding "$OUT"/build/bin/llama-tokenize "$OUT/bin/"
echo "[llama.cpp-ref] built into $OUT"
