Run host-side unit tests for arc_os kernel components.

## Steps

1. Configure tests if not already done:
   ```bash
   cmake -B build-test -DCMAKE_BUILD_TYPE=Debug
   ```
   (Host tests use system GCC, not the cross-compiler)

2. Build and run tests:
   ```bash
   cmake --build build-test --target test
   ```
   Or: `cd build-test && ctest --output-on-failure`

3. Report results: which tests passed, which failed, and failure details

## What Gets Tested

Host-side unit tests validate kernel algorithms without booting:
- Data structures (linked lists, hash tables, ring buffers)
- Memory allocator logic (bitmap, buddy)
- Scheduler algorithms
- Path resolution
- String/utility functions

These tests live in `tests/` and mirror the kernel source structure.

If no tests exist yet, suggest creating a test file for the current phase's code.
