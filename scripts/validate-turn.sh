#!/bin/bash
# validate-turn.sh — Stop hook: verify kernel cross-build + host tests after each turn
set -euo pipefail

# 1. Kernel cross-build
if [ -f toolchain-x86_64.cmake ]; then
    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake -Wno-dev 2>/dev/null
    if ! cmake --build build 2>&1; then
        echo "KERNEL BUILD FAILED — fix before continuing"
        exit 1
    fi
fi

# 2. Host-side unit tests
if [ -d tests/ ]; then
    cmake -B build_host -Wno-dev 2>/dev/null
    if ! cmake --build build_host 2>&1; then
        echo "HOST BUILD FAILED — fix before continuing"
        exit 1
    fi
    if ! ctest --test-dir build_host --output-on-failure 2>&1; then
        echo "TESTS FAILED — fix before continuing"
        exit 1
    fi
fi

echo "Build and tests passing."
