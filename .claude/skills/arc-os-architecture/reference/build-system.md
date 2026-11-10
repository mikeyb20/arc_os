# Build System Reference

The build files are the reference; this page only points at them (ADR-0020: docs don't restate code).
Commands for day-to-day use are in the top-level `README.md`.

| What | Where it is defined |
|---|---|
| Dev setup: apt packages, both pinned toolchains, Limine | `tools/dev-setup.sh`; pins in `tools/toolchain.lock` |
| Kernel toolchain (`x86_64-elf`, freestanding flags) | `toolchain-x86_64.cmake` |
| User-space toolchain (`x86_64-linux-musl`, static x86-64-v3) | `toolchain-musl.cmake`, prefix in `cmake/musl-prefix.cmake` |
| Top-level project, `iso` / `run` / `debug` targets | `CMakeLists.txt`, `tools/make-iso.sh`, `tools/run.sh`, `tools/debug.sh` |
| Kernel sources, flags (no SIMD, `ARC_KERNEL_DEBUG`) | `kernel/CMakeLists.txt`; linker script `kernel/arch/x86_64/linker.ld` |
| libarc and the base programs | `libc/CMakeLists.txt`; `arc_user_program()` in `userland/CMakeLists.txt`; `libc/user.ld` |
| Host unit tests (system GCC, label `host`) | `tests/CMakeLists.txt` |
| QEMU boot tests (label `qemu`) | `tests/qemu/CMakeLists.txt`, `tools/qemu-test.py` |
| Budgets and hygiene counts | `tests/budgets.txt`, `tools/budget-check.py`, `tools/hygiene.py`; see `docs/kernel/budgets.md` |
| User-space stack (musl, label `lane`) | `stack/CMakeLists.txt`, `tools/lane/build.sh`; conventions in `docs/plans/lane-l.md` |
| Ports of outside code | `ports/<name>/build.sh`; sources in `third_party/SOURCES.lock`, fetched by `tools/fetch-archived.sh` |
| CI | `.github/workflows/` (`ci.yml` cross build, host and QEMU tests; `lane.yml` stack; `toolchain.yml` toolchain releases) |
