# Shared settings for ports/<name>/build.sh (lane L, M2-02). Source it, don't run it:
#
#   . "$ROOT/tools/lane/port-env.sh"
#   port_begin <name> <prefix>      # sets SRC (archived source), WORK (scratch copy), PREFIX
#
# Compiler and flags match toolchain-musl.cmake: the pinned x86_64-linux-musl GCC
# (tools/dev-setup.sh musl), -march=x86-64-v3 (the arc_os CPU floor), static. CMake-based
# ports use that toolchain file itself (it reads ARC_MUSL_PREFIX, exported here).
# A port build never touches the network: sources come from third_party/src/<name>/,
# which tools/lane/build.sh fetches from the archive first (tools/fetch-archived.sh).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARC_MUSL_PREFIX="${ARC_MUSL_PREFIX:-$HOME/opt/cross-musl/bin/x86_64-linux-musl-}"
CC="${ARC_MUSL_PREFIX}gcc"
CXX="${ARC_MUSL_PREFIX}g++"
AR="${ARC_MUSL_PREFIX}ar"
RANLIB="${ARC_MUSL_PREFIX}ranlib"
STRIP="${ARC_MUSL_PREFIX}strip"
ARC_PORT_ARCH="-march=x86-64-v3"
export CC CXX AR RANLIB STRIP ARC_MUSL_PREFIX

# port_begin <name> <prefix>: check the toolchain and the archived source, copy the
# source to build-lane/work/<name>/ (upstream stays untouched), start an empty prefix.
port_begin() {
    local name=$1
    PREFIX=$2
    SRC="$ROOT/third_party/src/$name"
    WORK="$ROOT/build-lane/work/$name"
    [[ -x "$CC" ]] || { echo "[$name] no musl toolchain at $CC (tools/dev-setup.sh musl)" >&2; exit 1; }
    [[ -d "$SRC" ]] || { echo "[$name] no source in $SRC (tools/fetch-archived.sh $name)" >&2; exit 1; }
    rm -rf "$WORK" "$PREFIX"
    mkdir -p "$(dirname "$WORK")" "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/include"
    PREFIX="$(cd "$PREFIX" && pwd)"
    cp -a "$SRC" "$WORK"
    if [[ -d "$ROOT/ports/$name/patches" ]]; then
        local p
        for p in "$ROOT/ports/$name/patches"/*.patch; do
            [[ -e "$p" ]] || continue
            echo "[$name] patch $(basename "$p")"
            patch -d "$WORK" -p1 --forward < "$p"
        done
    fi
}

# port_check_static <file>...: fail if any program is dynamically linked.
port_check_static() {
    local f
    for f in "$@"; do
        if file "$f" | grep -q dynamic; then
            echo "$f is dynamically linked" >&2
            exit 1
        fi
    done
}
