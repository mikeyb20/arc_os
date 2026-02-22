#!/usr/bin/env bash
# build-cross-compiler.sh — Build x86_64-elf cross-compiler from source
# Builds: binutils 2.42, GCC 14.1.0
# Install prefix: $HOME/opt/cross
set -euo pipefail

BINUTILS_VERSION=2.42
GCC_VERSION=14.1.0
PREFIX="$HOME/opt/cross"
TARGET=x86_64-elf
JOBS=$(nproc)
SRC_DIR="$HOME/src/cross-build"

echo "=== arc_os cross-compiler build ==="
echo "Target:   $TARGET"
echo "Prefix:   $PREFIX"
echo "Jobs:     $JOBS"
echo "Sources:  $SRC_DIR"
echo ""

# Quick check: already built?
if command -v x86_64-elf-gcc &>/dev/null; then
    echo "x86_64-elf-gcc already found on PATH:"
    x86_64-elf-gcc --version | head -1
    echo "Delete $PREFIX to rebuild. Exiting."
    exit 0
fi

if [[ -x "$PREFIX/bin/x86_64-elf-gcc" ]]; then
    echo "x86_64-elf-gcc found at $PREFIX/bin/x86_64-elf-gcc"
    "$PREFIX/bin/x86_64-elf-gcc" --version | head -1
    echo "Add $PREFIX/bin to your PATH. Exiting."
    exit 0
fi

mkdir -p "$SRC_DIR" "$PREFIX"
cd "$SRC_DIR"

# --- Download sources ---
echo "=== Downloading sources ==="

if [[ ! -f "binutils-${BINUTILS_VERSION}.tar.xz" ]]; then
    wget -c "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"
else
    echo "binutils tarball already downloaded."
fi

if [[ ! -f "gcc-${GCC_VERSION}.tar.xz" ]]; then
    wget -c "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"
else
    echo "gcc tarball already downloaded."
fi

# --- Extract ---
echo "=== Extracting sources ==="

if [[ ! -d "binutils-${BINUTILS_VERSION}" ]]; then
    tar xf "binutils-${BINUTILS_VERSION}.tar.xz"
else
    echo "binutils already extracted."
fi

if [[ ! -d "gcc-${GCC_VERSION}" ]]; then
    tar xf "gcc-${GCC_VERSION}.tar.xz"
else
    echo "gcc already extracted."
fi

# --- Download GCC prerequisites (GMP, MPFR, MPC, ISL) ---
echo "=== Downloading GCC prerequisites ==="
cd "gcc-${GCC_VERSION}"
if [[ ! -d "gmp" ]]; then
    ./contrib/download_prerequisites
else
    echo "GCC prerequisites already downloaded."
fi
cd "$SRC_DIR"

# --- Build binutils ---
echo ""
echo "=== Building binutils ${BINUTILS_VERSION} ==="

rm -rf build-binutils
mkdir build-binutils
cd build-binutils

"../binutils-${BINUTILS_VERSION}/configure" \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --with-sysroot \
    --disable-nls \
    --disable-werror

make MAKEINFO=true -j"$JOBS"
make MAKEINFO=true install

cd "$SRC_DIR"

# --- Build GCC ---
echo ""
echo "=== Building GCC ${GCC_VERSION} ==="

export PATH="$PREFIX/bin:$PATH"

rm -rf build-gcc
mkdir build-gcc
cd build-gcc

"../gcc-${GCC_VERSION}/configure" \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --disable-nls \
    --enable-languages=c \
    --without-headers \
    --disable-shared \
    --disable-threads \
    --with-newlib \
    --disable-hosted-libstdcxx

make all-gcc -j"$JOBS"
make all-target-libgcc -j"$JOBS"
make install-gcc
make install-target-libgcc

cd "$SRC_DIR"

# --- Verify ---
echo ""
echo "=== Verification ==="
echo -n "x86_64-elf-gcc: "
"$PREFIX/bin/x86_64-elf-gcc" --version | head -1
echo -n "x86_64-elf-ld:  "
"$PREFIX/bin/x86_64-elf-ld" --version | head -1

echo ""
echo "=== SUCCESS ==="
echo "Cross-compiler installed to: $PREFIX/bin/"
echo ""
echo "Add to your shell profile:"
echo "  export PATH=\"\$HOME/opt/cross/bin:\$PATH\""
