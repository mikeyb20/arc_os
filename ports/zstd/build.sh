#!/usr/bin/env bash
# zstd 1.5.7 for arc_os (M2-02): libzstd.a, zstd.h, zstd_errors.h and the zstd CLI,
# static, built with the pinned musl toolchain. Offline; see PORT.md.
#
#   ports/zstd/build.sh <prefix>      (tools/lane/build.sh: build-lane/ports/zstd)
set -euo pipefail
. "$(cd "$(dirname "$0")/../.." && pwd)/tools/lane/port-env.sh"
port_begin zstd "${1:?usage: $0 <prefix>}"

# Upstream's own optimisation level (-O3) plus the CPU floor. No zlib, lzma or lz4 in the
# CLI (its Makefile would pick them up from the host otherwise); no legacy (pre-0.8) frames.
FLAGS=(CC="$CC" AR="$AR" CFLAGS="-O3 $ARC_PORT_ARCH" ZSTD_LEGACY_SUPPORT=0)
make -C "$WORK/lib" -j"$(nproc)" "${FLAGS[@]}" libzstd.a
make -C "$WORK/programs" -j"$(nproc)" "${FLAGS[@]}" LDFLAGS=-static \
    HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 zstd-release

cp "$WORK/lib/libzstd.a" "$PREFIX/lib/"
cp "$WORK/lib/zstd.h" "$WORK/lib/zstd_errors.h" "$PREFIX/include/"
cp "$WORK/programs/zstd" "$PREFIX/bin/"
"$STRIP" "$PREFIX/bin/zstd"
port_check_static "$PREFIX/bin/zstd"
echo "[zstd] installed into $PREFIX"
