# musl-cross-make configuration for the arc_os user-space toolchain (M2-01).
# Copied into the work tree by ports/musl-cross-make/build.sh. Versions match
# tools/toolchain.lock (binutils, GCC) and third_party/SOURCES.lock (the rest).

TARGET = x86_64-linux-musl

BINUTILS_VER = 2.42
GCC_VER = 14.1.0
MUSL_VER = 1.2.5
GMP_VER = 6.2.1
MPC_VER = 1.2.1
MPFR_VER = 4.1.0

# No Linux kernel headers: musl doesn't need them, and nothing in the lane
# includes <linux/...> (lane-l.md). A port that does gets Michael's OK first.
LINUX_VER =

# Every source is placed in sources/ from the archive before make runs; any
# attempt to download fails the build instead of reaching a third-party host.
DL_CMD = false

COMMON_CONFIG += CFLAGS="-g0 -O2" CXXFLAGS="-g0 -O2" LDFLAGS="-s"
COMMON_CONFIG += --disable-nls
GCC_CONFIG += --enable-languages=c,c++ --disable-multilib --disable-libsanitizer
BINUTILS_CONFIG += --disable-gprofng
