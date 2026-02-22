#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

LIMINE_DIR="$PROJECT_DIR/limine"
BUILD_DIR="$PROJECT_DIR/build"
ISO_ROOT="$BUILD_DIR/iso_root"

# Verify prerequisites
if [ ! -d "$LIMINE_DIR" ]; then
    echo "Error: limine/ directory not found. Run Chunk 0.4 to obtain Limine." >&2
    exit 1
fi
if [ ! -f "$BUILD_DIR/kernel.elf" ]; then
    echo "Error: build/kernel.elf not found. Build the kernel first." >&2
    exit 1
fi

# Create ISO directory structure
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot" "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"

# Copy kernel and config
cp "$BUILD_DIR/kernel.elf" "$ISO_ROOT/boot/kernel.elf"
cp "$PROJECT_DIR/limine.conf" "$ISO_ROOT/boot/limine/limine.conf"

# Copy Limine binaries
cp "$LIMINE_DIR/limine-bios.sys" "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-bios-cd.bin" "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/BOOTX64.EFI" "$ISO_ROOT/EFI/BOOT/"
cp "$LIMINE_DIR/BOOTIA32.EFI" "$ISO_ROOT/EFI/BOOT/"

# Create the ISO image
xorriso -as mkisofs \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "$BUILD_DIR/arc_os.iso" 2>/dev/null

# Install Limine BIOS stages into ISO
"$LIMINE_DIR/limine" bios-install "$BUILD_DIR/arc_os.iso" 2>/dev/null

echo "ISO created: $BUILD_DIR/arc_os.iso"
