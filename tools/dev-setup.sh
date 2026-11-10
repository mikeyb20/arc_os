#!/usr/bin/env bash
# dev-setup.sh — Install everything needed to build and test arc_os. Idempotent.
#
# Used by the cloud environment setup script, CI, the Dockerfile and Michael's laptop.
#
#   tools/dev-setup.sh            apt packages + both toolchains + Limine
#   tools/dev-setup.sh apt        apt packages only
#   tools/dev-setup.sh toolchain  pinned x86_64-elf toolchain only (kernel, libarc)
#   tools/dev-setup.sh musl       pinned x86_64-linux-musl toolchain only (user space, M2-01)
#   tools/dev-setup.sh limine     pinned Limine only
#
# Pins come from tools/toolchain.lock. Ubuntu 24.04's apt versions are the pin for host tools.
#
# Environment:
#   ARC_CROSS_DIR   where the toolchain goes (default: $HOME/opt/cross). If you change it, also
#                   set ARC_CROSS_PREFIX=$ARC_CROSS_DIR/bin/x86_64-elf- for CMake.
#   ARC_MUSL_DIR    where the musl toolchain goes (default: $HOME/opt/cross-musl). If you change
#                   it, also set ARC_MUSL_PREFIX=$ARC_MUSL_DIR/bin/x86_64-linux-musl- for CMake.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
# shellcheck source=toolchain.lock
source "$SCRIPT_DIR/toolchain.lock"

CROSS_DIR="${ARC_CROSS_DIR:-$HOME/opt/cross}"
MUSL_DIR="${ARC_MUSL_DIR:-$HOME/opt/cross-musl}"

APT_PACKAGES=(
    build-essential cmake ninja-build nasm
    qemu-system-x86 qemu-utils ovmf xorriso mtools dosfstools e2fsprogs gdisk
    musl-tools    # only for the static-PIE start test (userland/testprogs); the rest use the pinned musl
    strace python3 python3-pip python3-jsonschema jq zstd clang-format
    git curl ca-certificates file
    g++-14    # host GCC of the same major as the musl toolchain: llama.cpp reference build (M2-03)
)

log() { printf '[dev-setup] %s\n' "$*"; }
die() { printf '[dev-setup] error: %s\n' "$*" >&2; exit 1; }

as_root() {
    if [[ $EUID -eq 0 ]]; then "$@"; else sudo "$@"; fi
}

setup_apt() {
    local missing=()
    for p in "${APT_PACKAGES[@]}"; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "ok installed" || missing+=("$p")
    done
    if [[ ${#missing[@]} -eq 0 ]]; then
        log "apt packages already installed"
        return
    fi
    log "installing apt packages: ${missing[*]}"
    as_root apt-get update -qq
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${missing[@]}"
}

# install_toolchain <dir> <target> <asset> <url> <sha256> <lock-var>: download a pinned
# toolchain release asset, verify it and unpack it into <dir>, unless it is already there.
install_toolchain() {
    local dir=$1 target=$2 asset=$3 url=$4 sha=$5 var=$6
    local stamp="$dir/.arc-toolchain-sha256"
    [[ -n "$sha" ]] || die "$var is empty in tools/toolchain.lock (run the toolchain workflow first)"

    if [[ -f "$stamp" && "$(cat "$stamp")" == "$sha" && -x "$dir/bin/$target-gcc" ]]; then
        log "$target toolchain already installed in $dir"
    else
        command -v zstd >/dev/null || die "zstd is missing (run: tools/dev-setup.sh apt)"
        TMP_DL="$(mktemp -d)"
        trap 'rm -rf "$TMP_DL"' EXIT
        local tmp="$TMP_DL"
        log "downloading $asset"
        curl -fsSL --retry 4 --retry-delay 2 -o "$tmp/$asset" "$url"
        echo "$sha  $tmp/$asset" | sha256sum -c --quiet - \
            || die "SHA-256 mismatch for $asset"
        log "SHA-256 verified"
        rm -rf "$dir"
        mkdir -p "$dir"
        zstd -dc "$tmp/$asset" | tar -xf - -C "$dir"
        echo "$sha" > "$stamp"
        rm -rf "$tmp"
        log "$target toolchain installed in $dir"
    fi

    # Put the tools on PATH for every shell when we can (cloud sessions and Docker run as root).
    if [[ $EUID -eq 0 && -w /usr/local/bin ]]; then
        for t in "$dir"/bin/"$target"-*; do
            ln -sf "$t" /usr/local/bin/
        done
    elif ! command -v "$target-gcc" >/dev/null; then
        log "add to your shell profile: export PATH=\"$dir/bin:\$PATH\""
    fi
    "$dir/bin/$target-gcc" --version | head -1
}

setup_toolchain() {
    install_toolchain "$CROSS_DIR" "$TOOLCHAIN_TARGET" "$TOOLCHAIN_ASSET" "$TOOLCHAIN_URL" \
        "$TOOLCHAIN_SHA256" TOOLCHAIN_SHA256
}

setup_musl() {
    install_toolchain "$MUSL_DIR" "$MUSL_TOOLCHAIN_TARGET" "$MUSL_TOOLCHAIN_ASSET" "$MUSL_TOOLCHAIN_URL" \
        "$MUSL_TOOLCHAIN_SHA256" MUSL_TOOLCHAIN_SHA256
}

setup_limine() {
    local dir="$PROJECT_DIR/limine"
    if [[ -d "$dir/.git" && "$(git -C "$dir" rev-parse HEAD)" == "$LIMINE_COMMIT" ]]; then
        log "Limine $LIMINE_TAG already present"
    else
        log "fetching Limine $LIMINE_TAG"
        rm -rf "$dir"
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$LIMINE_TAG" "$LIMINE_REPO" "$dir"
        [[ "$(git -C "$dir" rev-parse HEAD)" == "$LIMINE_COMMIT" ]] \
            || die "Limine $LIMINE_TAG is not at the pinned commit $LIMINE_COMMIT"
    fi
    make -s -C "$dir" >/dev/null
    if ! cmp -s "$dir/limine.h" "$PROJECT_DIR/kernel/include/limine.h"; then
        cp "$dir/limine.h" "$PROJECT_DIR/kernel/include/limine.h"
        log "updated kernel/include/limine.h from Limine $LIMINE_TAG (commit it)"
    fi
}

case "${1:-all}" in
    all)       setup_apt; setup_toolchain; setup_musl; setup_limine ;;
    apt)       setup_apt ;;
    toolchain) setup_toolchain ;;
    musl)      setup_musl ;;
    limine)    setup_limine ;;
    *)         die "unknown step '$1' (all|apt|toolchain|musl|limine)" ;;
esac
log "done"
