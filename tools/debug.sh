#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake
cmake --build build
bash tools/make-iso.sh

echo "=== QEMU starting in debug mode ==="
echo "Connect GDB with:  gdb -ex 'target remote :1234' build/kernel.elf"
echo "===================================="

qemu-system-x86_64 \
    -cdrom build/arc_os.iso \
    -serial stdio \
    -m 256M \
    -no-reboot \
    -no-shutdown \
    -s -S
