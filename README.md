# arc_os

arc_os is a from-scratch x86_64 operating system for an **offline knowledge computer**. It installs from a
USB key, runs on anything from a 4 GB laptop to a strong desktop, and lets local LLMs (llama.cpp, CPU
only) search a large offline library and answer with citations. It works fully without internet.

The kernel is monolithic, written in C11 with NASM stubs, and boots with Limine. Programs use the Linux
x86_64 syscall ABI and System V process start, so static Linux/musl binaries run unchanged.

## Where things stand

- **Plan:** [`docs/plans/knowledge-os.md`](docs/plans/knowledge-os.md) has the goal and milestones M0–M5.
  The decisions it rests on are the ADRs in [`docs/decisions/`](docs/decisions/).
- **Progress:** [`docs/plans/work-blocks.md`](docs/plans/work-blocks.md) lists every work block and its
  status. One block is one PR; each block's spec is in `docs/plans/blocks/`.
- **History:** `overview.md` (phases 0–12) and `docs/implementation-plan.md` describe the work before the plan.

## Layout

```
kernel/        the kernel (arch/x86_64 behind arch/hal.h; boot, mm, proc, fs, drivers, net)
libc/          libarc, the small C library the base system links against
userland/      base programs on libarc: init, login, shell, coreutils, test programs
stack/         user-space services built with musl (arc-llm, shared libraries, lane tests)
ports/         build recipes for outside code (musl toolchain, llama.cpp, SQLite, zstd)
third_party/   archived, pinned outside sources (SOURCES.lock) and their licences
schemas/       JSON schemas for .arcpkg packages, models and packs
tests/         host unit tests, QEMU boot tests, target test programs
tools/         dev setup, ISO, QEMU test runner, hygiene and budget checks
docs/          plan, ADRs, block specs, kernel notes (budgets.md, helpers.md)
```

## Build and test

Ubuntu 24.04. One script installs the apt packages, both pinned toolchains (`x86_64-elf` for the
kernel and libarc, `x86_64-linux-musl` for user space) and Limine:

```bash
tools/dev-setup.sh
```

Kernel, libarc, base programs and the bootable ISO:

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake
cmake --build build
cmake --build build --target iso      # build/arc_os.iso
cmake --build build --target run      # boot it in QEMU, serial on stdio
cmake --build build --target debug    # QEMU waits for GDB on :1234
```

Tests (the QEMU tests use the ISO pieces from `build/`):

```bash
cmake -B build-tests -G Ninja && cmake --build build-tests
ctest --test-dir build-tests -L host   # host unit tests
ctest --test-dir build-tests -L qemu   # boot tests in QEMU
python3 tools/hygiene.py               # ADR-0020 ratchet counts
```

User-space stack (lane L, built and tested on Linux first, see
[`docs/plans/lane-l.md`](docs/plans/lane-l.md)):

```bash
tools/lane/build.sh
ctest --test-dir build-lane/stack -L lane
```

CI (`.github/workflows/`) runs the cross build, host tests and QEMU tests on every PR, and the lane
build and tests when lane files change.

## License

MIT. Outside code keeps its own licence; see [`third_party/LICENSES.md`](third_party/LICENSES.md).
