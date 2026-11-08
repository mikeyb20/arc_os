# M0-01 — Dev environment, pinned toolchain and CI

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | I | — | M0-02, M0-03, everything |

**Goal.** Every cloud session, CI run and Michael's laptop builds with the same pinned tools, and no session
ever builds GCC or waits on a blocked download host.

**Read first.**
- `tools/setup-deps.sh`, `tools/build-cross-compiler.sh`, `toolchain-x86_64.cmake`, `CMakeLists.txt`,
  `tests/CMakeLists.txt`
- `docs/plans/knowledge-os.md` §3.4
- `docs/plans/work-blocks.md` §8 (cloud constraints)

**Build.**
1. Find out whether a cloud session can download a release asset of `mikeyb20/arc_os` (`gh release download`
   through the proxy; `api.github.com` works only for repos attached to the session). Record the answer in Notes.
   If it can't, use the fallback in step 3.
2. `tools/toolchain.lock`: binutils 2.42 and GCC 14.1.0 tarball URLs with SHA-256. `.github/workflows/toolchain.yml`
   (manual trigger) downloads them, checks the hashes, builds `x86_64-elf` binutils + GCC (C only), packs
   `x86_64-elf-toolchain-<gcc>-<binutils>.tar.zst` and uploads it to release `toolchain-v1` on arc_os.
3. Fallback when release downloads are blocked: the same tarball, split into parts under 50 MB, committed to the
   archive repo chosen in M0-03 and fetched with `git clone --depth 1`.
4. `tools/dev-setup.sh` (idempotent, used by the cloud setup script, CI, the Dockerfile and the laptop):
   apt packages `build-essential cmake ninja-build nasm qemu-system-x86 qemu-utils ovmf xorriso mtools dosfstools
   e2fsprogs gdisk musl-tools strace python3 python3-pip jq zstd`; fetch and verify the toolchain into
   `$HOME/opt/cross`; fetch Limine at the commit pinned in `tools/setup-deps.sh` (pin it if it isn't).
5. `toolchain-x86_64.cmake`: cross prefix from `ARC_CROSS_PREFIX` (env or cache variable), default unchanged
   (`$HOME/opt/cross/bin/x86_64-elf-`).
6. Label the existing host tests `host` in ctest.
7. `.github/workflows/ci.yml` on pull requests and pushes to master: dev-setup (toolchain cached) → cross build
   (kernel, libc, userland, ISO) → host tests (`ctest -L host`). Upload the ISO as an artifact.
8. `Dockerfile` (ubuntu:24.04 + `tools/dev-setup.sh`) for building on the laptop.
9. Propose the cloud environment setup script (one line: `bash tools/dev-setup.sh`) to Michael on an environment
   card. Use a new personal environment, not Default.

**Done when.**
- [ ] CI is green on this block's PR (build + host tests).
- [x] The toolchain asset exists and `tools/dev-setup.sh` verifies its SHA-256.
- [ ] A fresh cloud session has `x86_64-elf-gcc`, `nasm`, `qemu-system-x86_64` and `xorriso` with no manual steps.

**Watch out.**
- Blocked from cloud sessions: ftp.gnu.org, musl.libc.org, download.kiwix.org, huggingface.co. GitHub git clones
  work. GitHub Actions runners can reach everything, so downloads happen there.
- Don't change compiler flags here; M1-01 does that.
- Ubuntu 24.04 apt versions are the pin for host tools; record `qemu-system-x86_64 --version` in Notes.

**Notes.**
- Done in PR #3 (branch `claude/project-thread-amkkol`; the session harness fixes the branch name).
- Release downloads work from cloud sessions: plain `https://github.com/mikeyb20/arc_os/releases/download/...`
  URLs go through the proxy (arc_os is public, so no `gh`/API call is needed). The step 3 fallback isn't needed.
- `toolchain-v1` asset: `x86_64-elf-toolchain-14.1.0-2.42.tar.zst`, 241 MB unpacked, SHA-256 in `tools/toolchain.lock`.
  Built in ~16 min on an Actions runner; installs in ~2 s in a cloud session. GNU source hashes were confirmed
  by the runner. The toolchain is relocatable (smoke test runs it from a moved directory).
- Limine pinned to `v8.7.0-binary` = `aad3edd3`; its `limine.h` is identical to the committed one.
- Host tool pins (Ubuntu 24.04 apt): `QEMU emulator version 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18)`.
- Host tests were red on master: `test_vmm` ran the real `cli`/`popf` spinlock in user space (SIGSEGV) and
  `test_fat32` had no disk image. Fixed with a spinlock stub and a ctest fixture that builds the image.
  All 49 tests carry the `host` label.
- Cross build + ISO checked in a cloud session with the downloaded toolchain; the ISO boots in QEMU to
  "Preemptive multitasking active".
- For M0-02: `ctest -L qemu` is still empty; CI already installs qemu/ovmf through `dev-setup.sh`.
- For M0-03: the toolchain workflow fetches GNU sources but doesn't archive them; M0-03 still archives
  binutils/GCC (+ GMP/MPFR/MPC/ISL from `download_prerequisites`).
