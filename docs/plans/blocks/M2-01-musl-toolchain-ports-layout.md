# M2-01 — musl cross toolchain, ports layout, C++ test

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | L | M2-00a, M0-03 | M2-02, M2-03, M2-04 |

**Goal.** A pinned `x86_64-linux-musl` GCC with libstdc++, built from the archive and installed by
`tools/dev-setup.sh`, plus the lane's skeleton: `ports/`, `stack/`, the strace wrapper and `lane.yml`. The M0-04
target programs, built with it, pass on Linux inside the syscall inventory.

**Read first.**
- `docs/plans/lane-l.md`
- `tools/toolchain.lock`, `.github/workflows/toolchain.yml`, `tools/dev-setup.sh`, `tools/fetch-archived.sh`
- `third_party/SOURCES.lock` entries `gcc`, `binutils`, `gmp`, `mpfr`, `mpc`, `musl`, `musl-cross-make`
- `.claude/skills/port-package/SKILL.md`, `tests/target/` (the three programs), Notes of M0-03 and M0-04

**Build.**
1. `ports/musl-cross-make/`: `PORT.md`, `config.mak` and `build.sh`. The archived musl-cross-make commit knows
   GCC 14.2/14.3 and binutils 2.44, not our pins, so `build.sh` writes the `hashes/*.sha1` it expects from our
   SHA-256-verified tarballs (generated at build time, not committed: the SHA-256 in `SOURCES.lock` is the trust
   anchor) and copies its `patches/gcc-14.2.0/` to `patches/gcc-14.1.0/` (all 10 apply to 14.1.0 unchanged;
   `PORT.md` says which matter on x86_64). musl 1.2.5 uses the commit's own patches. `config.mak`:
   `TARGET = x86_64-linux-musl`, the six versions, `LINUX_VER =` (empty: no kernel headers),
   `GCC_CONFIG += --enable-languages=c,c++ --disable-multilib`, `-g0 -O2`, and `DL_CMD = false` so any download
   attempt fails. `tools/archive.py fetch --cache-only` verifies tarballs without unpacking them.
2. `tools/toolchain.lock`: `MUSL_TOOLCHAIN_TARGET`, `MUSL_TOOLCHAIN_RELEASE=toolchain-musl-v1`, `_ASSET`, `_URL`,
   `_SHA256` (filled from the workflow's output). `.github/workflows/toolchain.yml` gets a second job, `musl`, in
   the same pattern as the existing one (skip if the asset exists, build, relocated smoke test with a C and a C++
   static program, pack with zstd, publish). Sources come only from `tools/fetch-archived.sh` into the
   musl-cross-make `sources/` directory; `sources/config.sub` is copied from the archived GCC tree, so make never
   fetches it.
3. `tools/dev-setup.sh musl`: installs the asset into `$HOME/opt/cross-musl` with the same stamp-and-verify logic as
   the elf toolchain (one function with parameters, not a copy). `all` includes it.
4. `toolchain-musl.cmake` (`-march=x86-64-v3`, `-static`, `CMAKE_FIND_ROOT_PATH` set to `build-lane/ports`),
   `stack/CMakeLists.txt` with `lane_test(NAME … COMMAND …)` (label `lane`; the command goes through
   `tools/lane/run.sh`) and `lane_driver_test()` for drivers, `stack/README.md` (the libraries table from
   lane-l.md, empty rows for now), `stack/budgets.txt`, `ports/README.md` (one paragraph pointing at the
   port-package skill and lane-l.md). `tools/budget-check.py` also reads `<suite>.budget.log` files, and
   `tools/lane/sizes.py` writes binary sizes in that format.
5. `tools/lane/run.sh <log-name> <cmd…>`: `exec "$@"` normally; with `ARC_LANE_LOGS` set,
   `strace -f -qq -s 200 -o "$ARC_LANE_LOGS/<log-name>.strace" -- "$@"`, keeping the exit status.
   `tools/lane/build.sh`: fetch the archived sources the ports need, run each `ports/*/build.sh` that exists (none
   yet besides the toolchain), configure and build `stack/` into `build-lane/stack`.
6. First lane tests: `tests/target/libc_basic.c`, `pthread_basic.c` and `cxx_basic.cpp` built by `stack/` with the
   new toolchain (no copies; `stack/CMakeLists.txt` points at the files) and run as `lane.libc_basic`,
   `lane.pthread_basic`, `lane.cxx_basic`. Budget: `cxx_basic_bytes` (static C++ binary size).
7. `.github/workflows/lane.yml`: on pull requests and pushes touching `stack/**`, `ports/**`, `tools/lane/**`,
   `toolchain-musl.cmake`, `tests/target/**`, `tools/syscall-inventory.py` or the workflow. Steps:
   `dev-setup.sh apt` and `musl`, `tools/lane/build.sh`, `ctest -L lane` with `ARC_LANE_LOGS` set, the syscall check
   (lane-l.md), `budget-check.py --budgets stack/budgets.txt`, upload the logs on failure.
8. `tools/hygiene.py` and the CI format check include `stack/` (counts stay at master's level: the tree is new).
   `.gitignore`: `build-lane/`.

**Done when.**
- [ ] The `musl` toolchain job is green and `tools/toolchain.lock` pins the asset's SHA-256.
- [ ] A fresh cloud session gets the musl toolchain from `tools/dev-setup.sh` with no third-party host.
- [ ] `lane.yml` is green: the three target programs pass and the syscall check passes.
- [ ] `gcc -v` of the new toolchain shows 14.1.0 and `--enable-languages=c,c++`; `file` shows the test binaries are
      static.

**Watch out.**
- If GCC 14.1.0 won't build with the copied patches, stop and ask Michael: the alternative is GCC 14.2.0 and
  binutils 2.44 (the pins musl-cross-make tests), which needs new archive entries and splits the two toolchains'
  versions. Default while he decides: keep trying 14.1.0 for at most one more attempt.
- No Linux kernel headers in the toolchain. If a port includes `<linux/…>`, record which header and why in its
  `PORT.md`; the archived `linux-uapi-ref` is reference only and must not be installed into the sysroot without
  Michael's OK.
- The toolchain build takes 30–60 minutes on a runner. Never build it in a session; sessions install the asset.
- This closes the M0-03 follow-up for the musl side only (sources from the archive). The elf toolchain job still
  downloads from GNU mirrors; leave it.

**Notes.**
-
