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
- [ ] The toolchain asset exists and `tools/dev-setup.sh` verifies its SHA-256.
- [ ] A fresh cloud session has `x86_64-elf-gcc`, `nasm`, `qemu-system-x86_64` and `xorriso` with no manual steps.

**Watch out.**
- Blocked from cloud sessions: ftp.gnu.org, musl.libc.org, download.kiwix.org, huggingface.co. GitHub git clones
  work. GitHub Actions runners can reach everything, so downloads happen there.
- Don't change compiler flags here; M1-01 does that.
- Ubuntu 24.04 apt versions are the pin for host tools; record `qemu-system-x86_64 --version` in Notes.

**Notes.**
-
