#!/usr/bin/env bash
# llama.cpp b12198 for arc_os (M2-03): libllama.a, the libggml*.a libraries, llama.h and the
# ggml headers it includes, and the tools llama-completion, llama-embedding and llama-tokenize,
# static, x86-64-v3, built with the pinned musl toolchain (toolchain-musl.cmake). Offline;
# options in options.cmake, reasons in PORT.md.
#
#   ports/llama.cpp/build.sh <prefix>      (tools/lane/build.sh: build-lane/ports/llama.cpp)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../../tools/lane/port-env.sh"
port_begin llama.cpp "${1:?usage: $0 <prefix>}"

TOOLS=(llama-completion llama-embedding llama-tokenize)
cmake -S "$WORK" -B "$WORK/build" -G Ninja -C "$HERE/options.cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain-musl.cmake"
cmake --build "$WORK/build" -j"$(nproc)" --target llama "${TOOLS[@]}"

cp "$WORK/build/src/libllama.a" "$WORK"/build/ggml/src/libggml*.a "$PREFIX/lib/"
# llama.h and exactly the ggml headers it includes (gcc -MM lists them).
"$CC" -x c -MM -I"$WORK/include" -I"$WORK/ggml/include" "$WORK/include/llama.h" |
    tr ' \\' '\n\n' | grep '\.h$' | xargs -I{} cp {} "$PREFIX/include/"
for t in "${TOOLS[@]}"; do
    cp "$WORK/build/bin/$t" "$PREFIX/bin/"
    "$STRIP" "$PREFIX/bin/$t"
done
port_check_static "$PREFIX"/bin/*
echo "[llama.cpp] installed into $PREFIX"
