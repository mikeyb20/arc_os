#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# Clone Limine if not present
if [ ! -d "limine" ]; then
    echo "Cloning Limine v8.x binary branch..."
    git clone https://github.com/limine-bootloader/limine.git \
        --branch=v8.x-binary --depth=1 limine
fi

# Build limine CLI tool
echo "Building Limine CLI tool..."
make -C limine

# Copy limine.h into kernel includes
cp limine/limine.h kernel/include/limine.h

echo "Dependencies ready."
