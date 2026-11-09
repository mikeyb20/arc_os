# musl-cross-make — the x86_64-linux-musl toolchain

| | |
|---|---|
| What | GCC 14.1.0 (C, C++, libstdc++) + binutils 2.42 + musl 1.2.5 for `x86_64-linux-musl`, static |
| Built by | `musl-cross-make` commit `5e8042e63de29f8473fc3b0b831a587ba284e1a8` (MIT) |
| Sources | `third_party/SOURCES.lock`: `gcc`, `binutils`, `gmp` 6.2.1, `mpfr` 4.1.0, `mpc` 1.2.1, `musl`, `musl-cross-make` (SHA-256 pinned, fetched from the archive) |
| Licences | GCC: GPL-3.0-or-later with the GCC runtime exception (libstdc++ and libgcc may be linked into any program); binutils: GPL-3.0-or-later (build host only); musl: MIT |
| Why | knowledge-os.md §3.4: user space is `x86_64-linux-musl` GCC + libstdc++, static. Programs that embed a ported library link musl (lane-l.md) |
| Output | Release asset `toolchain-musl-v1` (see `tools/toolchain.lock`), installed by `tools/dev-setup.sh musl` into `$HOME/opt/cross-musl` |

## How it is built

`build.sh <output-dir>` copies the archived musl-cross-make tree into `build-lane/musl-cross-make/`, adds
`config.mak` from this folder, puts the archived tarballs in its `sources/` directory and runs `make` and
`make install`. `.github/workflows/toolchain.yml` (job `musl`) runs it once and publishes the result; sessions only
install the asset. Nothing is downloaded during the build: `DL_CMD = false` makes any attempt fail.

The commit was written for GCC 14.2/14.3 and binutils 2.44. Three things bridge it to our pins (the same GCC and
binutils as the kernel toolchain, already archived):

- **SHA-1 files.** musl-cross-make expects `hashes/<tarball>.sha1`. `build.sh` writes them from our tarballs after
  `tools/archive.py` has checked their SHA-256, which is the real trust anchor.
- **GCC patches.** Its `patches/gcc-14.2.0/` set (10 patches) is copied to `patches/gcc-14.1.0/`. All 10 apply to
  14.1.0 unchanged (checked 2026-11-08 with `build.sh --prepare-only`). The ones that matter on x86_64 are
  `0001-ssp_nonshared`, `0002-posix_memalign`, `0004-static-pie` and `0006-cow-libstdc++v3`; the rest touch other
  architectures and are harmless here. binutils 2.44's patches are for SH, RISC-V and s390x only, so 2.42 builds
  without any.
- **config.sub.** musl-cross-make downloads a `config.sub`; `build.sh` takes GCC 14.1.0's own copy instead.

## Options (`config.mak`)

| Option | Why |
|---|---|
| `LINUX_VER =` (empty) | No Linux kernel headers in the sysroot. musl doesn't need them; a port that includes `<linux/…>` must say why in its `PORT.md` and get Michael's OK |
| `DL_CMD = false` | Offline guarantee: a missing source fails the build |
| `--enable-languages=c,c++` | libstdc++ for llama.cpp |
| `--disable-multilib` | 64-bit only |
| `--disable-libsanitizer` | Sanitizer runtimes are unused and don't build cleanly against musl |
| `--disable-nls`, `-g0 -O2`, `LDFLAGS=-s` | Smaller toolchain asset; no translated messages |
| `--disable-gprofng` | binutils' profiler is unused and needs host libraries |

## Known gaps

- Not reproducible bit for bit (GCC embeds build paths in a few places). The asset is pinned by SHA-256 instead,
  like the elf toolchain.
