#!/usr/bin/env bash
# Build a bootable arc_os ISO from build/.
#
# Usage: make-iso.sh [-o OUTPUT.iso] [-c "kernel cmdline"] [-B] [EXTRA_MODULE ...]
#   -o  output ISO path (default build/arc_os.iso)
#   -c  kernel command line, e.g. "arc.test=smoke" for kernel test mode
#   -B  bring-up stick (M1-04, default build/arc_os-bringup.iso): a 10 s boot
#       menu whose default entry is the boot report with self-test and benchmark,
#       then the boot report alone, the same report as QR codes, the kernel log
#       as QR codes (M1-04c), then normal boot (docs/hardware/bringup.md)
#   EXTRA_MODULE  files added under /boot/ and loaded as Limine modules after
#                 the standard programs (test programs, suite lists)
# With no arguments it builds the normal build/arc_os.iso.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

LIMINE_DIR="$PROJECT_DIR/limine"
BUILD_DIR="$PROJECT_DIR/build"
OUTPUT="$BUILD_DIR/arc_os.iso"
CMDLINE=""
BRINGUP=0
while getopts "o:c:B" opt; do
    case "$opt" in
        o) OUTPUT="$OPTARG" ;;
        c) CMDLINE="$OPTARG" ;;
        B) BRINGUP=1 ;;
        *) echo "usage: $0 [-o output.iso] [-c cmdline] [-B] [extra modules...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
EXTRA_MODULES=("$@")
if [ "$BRINGUP" = 1 ]; then
    if [ -n "$CMDLINE" ] || [ ${#EXTRA_MODULES[@]} -gt 0 ]; then
        echo "Error: -B takes no -c or extra modules." >&2
        exit 2
    fi
    case "$OUTPUT" in "$BUILD_DIR/arc_os.iso") OUTPUT="$BUILD_DIR/arc_os-bringup.iso" ;; esac
fi

# One staging directory per output, so several ISOs can be built in parallel
ISO_ROOT="$BUILD_DIR/iso_root-$(basename "$OUTPUT" .iso)"

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

# Copy kernel and config. The arc_os entry is the last one in limine.conf, so
# the command line and extra modules are appended to it.
cp "$BUILD_DIR/kernel.elf" "$ISO_ROOT/boot/kernel.elf"
CONF="$ISO_ROOT/boot/limine/limine.conf"
cp "$PROJECT_DIR/limine.conf" "$CONF"
if [ -n "$CMDLINE" ]; then
    echo "    cmdline: $CMDLINE" >> "$CONF"
fi
for mod in "${EXTRA_MODULES[@]}"; do
    if [ ! -f "$mod" ]; then
        echo "Error: extra module '$mod' not found." >&2
        exit 1
    fi
    name="$(basename "$mod")"
    cp "$mod" "$ISO_ROOT/boot/$name"
    echo "    module_path: boot():/boot/$name" >> "$CONF"
done

# Bring-up stick: four more entries that repeat the arc_os entry's lines with a
# diagnostic command line, listed before it so the first one is the default
if [ "$BRINGUP" = 1 ]; then
    ENTRY="$(sed -n '/^\/arc_os$/,$p' "$PROJECT_DIR/limine.conf" | tail -n +2)"
    {
        echo "timeout: 10"
        echo "default_entry: 1"
        echo
        printf '/arc_os boot report, self-test and benchmark\n%s\n    cmdline: arc.selftest=1 arc.bench=1\n\n' "$ENTRY"
        printf '/arc_os boot report\n%s\n    cmdline: arc.report=1\n\n' "$ENTRY"
        printf '/arc_os boot report as QR codes (with self-test and benchmark)\n%s\n    cmdline: arc.qr=1 arc.selftest=1 arc.bench=1\n\n' "$ENTRY"
        printf '/arc_os kernel log as QR codes\n%s\n    cmdline: arc.qr=log\n\n' "$ENTRY"
        echo "/arc_os normal boot"
        echo "$ENTRY"
    } > "$CONF"
fi

# Copy all userland binaries
BINARIES="init login shell hello echo cat wc head tail touch mkdir rm ls stat chmod chown cp mv ps kill uname grep free"
for bin in $BINARIES; do
    if [ -f "$BUILD_DIR/$bin" ]; then
        cp "$BUILD_DIR/$bin" "$ISO_ROOT/boot/$bin"
    fi
done

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
    "$ISO_ROOT" -o "$OUTPUT" 2>/dev/null

# Install Limine BIOS stages into ISO
"$LIMINE_DIR/limine" bios-install "$OUTPUT" 2>/dev/null

echo "ISO created: $OUTPUT"
