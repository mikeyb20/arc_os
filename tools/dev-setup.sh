#!/usr/bin/env bash
# dev-setup.sh — Install everything needed to build and test arc_os. Idempotent.
#
# Used by the cloud environment setup script, CI, the Dockerfile and Michael's laptop.
#
#   tools/dev-setup.sh            apt packages + toolchain + Limine
#   tools/dev-setup.sh apt        apt packages only
#   tools/dev-setup.sh toolchain  pinned x86_64-elf toolchain only
#   tools/dev-setup.sh limine     pinned Limine only
#
# Pins come from tools/toolchain.lock. Ubuntu 24.04's apt versions are the pin for host tools.
#
# Environment:
#   ARC_CROSS_DIR   where the toolchain goes (default: $HOME/opt/cross). If you change it, also
#                   set ARC_CROSS_PREFIX=$ARC_CROSS_DIR/bin/x86_64-elf- for CMake.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
# shellcheck source=toolchain.lock
source "$SCRIPT_DIR/toolchain.lock"

CROSS_DIR="${ARC_CROSS_DIR:-$HOME/opt/cross}"
STAMP="$CROSS_DIR/.arc-toolchain-sha256"

APT_PACKAGES=(
    build-essential cmake ninja-build nasm
    qemu-system-x86 qemu-utils ovmf xorriso mtools dosfstools e2fsprogs gdisk
    musl-tools strace python3 python3-pip jq zstd clang-format
    git curl ca-certificates
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

setup_toolchain() {
    [[ -n "$TOOLCHAIN_SHA256" ]] || die "TOOLCHAIN_SHA256 is empty in tools/toolchain.lock (run the toolchain workflow first)"

    if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$TOOLCHAIN_SHA256" && -x "$CROSS_DIR/bin/$TOOLCHAIN_TARGET-gcc" ]]; then
        log "toolchain already installed in $CROSS_DIR"
    else
        command -v zstd >/dev/null || die "zstd is missing (run: tools/dev-setup.sh apt)"
        TMP_DL="$(mktemp -d)"
        trap 'rm -rf "$TMP_DL"' EXIT
        local tmp="$TMP_DL"
        log "downloading $TOOLCHAIN_ASSET"
        curl -fsSL --retry 4 --retry-delay 2 -o "$tmp/$TOOLCHAIN_ASSET" "$TOOLCHAIN_URL"
        echo "$TOOLCHAIN_SHA256  $tmp/$TOOLCHAIN_ASSET" | sha256sum -c --quiet - \
            || die "SHA-256 mismatch for $TOOLCHAIN_ASSET"
        log "SHA-256 verified"
        rm -rf "$CROSS_DIR"
        mkdir -p "$CROSS_DIR"
        zstd -dc "$tmp/$TOOLCHAIN_ASSET" | tar -xf - -C "$CROSS_DIR"
        echo "$TOOLCHAIN_SHA256" > "$STAMP"
        log "toolchain installed in $CROSS_DIR"
    fi

    # Put the tools on PATH for every shell when we can (cloud sessions and Docker run as root).
    if [[ $EUID -eq 0 && -w /usr/local/bin ]]; then
        for t in "$CROSS_DIR"/bin/"$TOOLCHAIN_TARGET"-*; do
            ln -sf "$t" /usr/local/bin/
        done
    elif ! command -v "$TOOLCHAIN_TARGET-gcc" >/dev/null; then
        log "add to your shell profile: export PATH=\"$CROSS_DIR/bin:\$PATH\""
    fi
    "$CROSS_DIR/bin/$TOOLCHAIN_TARGET-gcc" --version | head -1
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
    all)       setup_apt; setup_toolchain; setup_limine ;;
    apt)       setup_apt ;;
    toolchain) setup_toolchain ;;
    limine)    setup_limine ;;
    *)         die "unknown step '$1' (all|apt|toolchain|limine)" ;;
esac
log "done"
