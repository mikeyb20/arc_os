#!/usr/bin/env bash
# Build the x86_64-linux-musl cross toolchain (GCC + libstdc++, binutils, musl)
# with the archived musl-cross-make, offline (M2-01).
#
#   ports/musl-cross-make/build.sh <output-dir> [--prepare-only]
#
# Sources come only from the archive (tools/archive.py, SHA-256 checked) and are
# placed in musl-cross-make's sources/ directory, so make never downloads.
# The archived musl-cross-make commit knows GCC 14.2/14.3 and binutils 2.44, not
# our pins, so this script also writes the SHA-1 files it expects (from our
# verified tarballs), carries its gcc-14.2.0 patches over to 14.1.0, and takes
# config.sub from the archived GCC tree. Work happens in build-lane/musl-cross-make.
# --prepare-only stops after the sources are extracted and patched.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:?usage: $0 <output-dir> [--prepare-only]}"
PREPARE_ONLY="${2:-}"
WORK="$ROOT/build-lane/musl-cross-make"
CACHE="$ROOT/third_party/cache"

log() { printf '[musl-toolchain] %s\n' "$*"; }

python3 "$ROOT/tools/archive.py" fetch musl-cross-make
python3 "$ROOT/tools/archive.py" fetch --cache-only gcc binutils gmp mpfr mpc musl

rm -rf "$WORK"
mkdir -p "$(dirname "$WORK")"
cp -a "$ROOT/third_party/src/musl-cross-make" "$WORK"
cp "$HERE/config.mak" "$WORK/config.mak"
mkdir -p "$WORK/sources"

for f in gcc-14.1.0.tar.xz binutils-2.42.tar.xz gmp-6.2.1.tar.bz2 mpfr-4.1.0.tar.bz2 \
         mpc-1.2.1.tar.gz musl-1.2.5.tar.gz; do
    [[ -f "$CACHE/$f" ]] || { log "missing $CACHE/$f"; exit 1; }
    cp "$CACHE/$f" "$WORK/sources/$f"
    (cd "$CACHE" && sha1sum "$f") > "$WORK/hashes/$f.sha1"
done

# make rebuilds a source whose .sha1 is newer; the tarballs were verified above.
touch "$WORK"/sources/*

# config.sub: musl-cross-make would download it; GCC 14.1.0's copy knows x86_64-linux-musl.
tar -xJf "$CACHE/gcc-14.1.0.tar.xz" -O gcc-14.1.0/config.sub > "$WORK/sources/config.sub"
chmod +x "$WORK/sources/config.sub"

# GCC patches: the commit's gcc-14.2.0 set, carried to 14.1.0 (see PORT.md).
[[ -d "$WORK/patches/gcc-14.1.0" ]] || cp -a "$WORK/patches/gcc-14.2.0" "$WORK/patches/gcc-14.1.0"

cd "$WORK"
make extract_all
log "sources extracted and patched in $WORK"
[[ "$PREPARE_ONLY" == "--prepare-only" ]] && exit 0

make -j"$(nproc)"
make install OUTPUT="$OUT"
log "installed in $OUT"
