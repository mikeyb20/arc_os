#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake
cmake --build build
bash tools/make-iso.sh

# Create test disk if it doesn't exist
TEST_DISK="$PROJECT_DIR/build/test_disk.img"
if [ ! -f "$TEST_DISK" ]; then
    echo "Creating 32MB test disk with MBR signature..."
    dd if=/dev/zero of="$TEST_DISK" bs=1M count=32 2>/dev/null
    # Write MBR signature (0x55AA at offset 510)
    printf '\x55\xAA' | dd of="$TEST_DISK" bs=1 seek=510 conv=notrunc 2>/dev/null
fi

qemu-system-x86_64 \
    -cdrom build/arc_os.iso \
    -serial stdio \
    -m 256M \
    -no-reboot \
    -no-shutdown \
    -boot d \
    -drive file="$TEST_DISK",format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0
