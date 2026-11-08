# Third-party licenses

One row per entry in [`SOURCES.lock`](SOURCES.lock) (M0-03). The archive (release assets on
mikeyb20/arc_os, which is public) holds openly licensed material only; private material goes in the
private repo (work-blocks.md §6).

**Where it ships:** *Build host only* = used to build arc_os, never copied onto a machine. *On the
install key* = copied onto the USB key, for example as the archived sources the plan requires
(knowledge-os.md §3.4). *Inside arc_os* = linked into or installed with arc_os programs, so its
license notice must ship with them. *Tests only* = CI and QEMU tests.

Every archived source tarball also goes on the install key, so the sources of all GPL and LGPL code
used to build arc_os ship alongside it.

| Component | Version | License | Where it ships | Purpose |
|---|---|---|---|---|
| binutils | 2.42 | GPL-3.0-or-later | Build host only | Assembler and linker for both cross toolchains (same pin as tools/toolchain.lock) |
| gcc | 14.1.0 | GPL-3.0-or-later WITH GCC-exception-3.1 | Build host only | C/C++ compilers and libstdc++ (same pin as tools/toolchain.lock) |
| gmp | 6.2.1 | LGPL-3.0-or-later OR GPL-2.0-or-later | Build host only | GCC prerequisite (version from gcc-14.1.0 contrib/download_prerequisites) |
| mpfr | 4.1.0 | LGPL-3.0-or-later | Build host only | GCC prerequisite (version from gcc-14.1.0 contrib/download_prerequisites) |
| mpc | 1.2.1 | LGPL-3.0-or-later | Build host only | GCC prerequisite (version from gcc-14.1.0 contrib/download_prerequisites) |
| isl | 0.24 | MIT | Build host only | GCC prerequisite (version from gcc-14.1.0 contrib/download_prerequisites) |
| musl | 1.2.5 | MIT | Inside arc_os (linked or installed) | C library for ported software (linked statically into arc_os programs) |
| musl-cross-make | 5e8042e63de2 | MIT | Build host only | Build recipe for the x86_64-linux-musl cross toolchain (M2-01) |
| limine-binary | 8.7.0 | BSD-2-Clause | On the install key | Bootloader binaries used by tools/make-iso.sh (tag v8.7.0-binary, same commit as tools/toolchain.lock) |
| limine | 8.7.0 | BSD-2-Clause | On the install key | Bootloader source matching the binary release |
| llama.cpp | b12198 | MIT | Inside arc_os (linked or installed) | CPU inference engine behind arc-llm (M2-03) |
| sqlite | 3.54.1 | blessing (public domain) | Inside arc_os (linked or installed) | SQLite amalgamation; FTS5 is built in with -DSQLITE_ENABLE_FTS5 (M2-02) |
| zstd | 1.5.7 | BSD-3-Clause OR GPL-2.0-only | Inside arc_os (linked or installed) | Compression for .arcpkg and pack doc stores (hash matches upstream .sha256 file) |
| uacpi | 7.0.0 | MIT | Inside arc_os (linked or installed) | ACPI/AML interpreter (M4-11) |
| e2fsprogs | 1.47.4 | GPL-2.0-only (libext2fs LGPL-2.0-only) | Inside arc_os (linked or installed) | mke2fs and e2fsck port (M5-01); host-side image building |
| stb_image | 4e9b368b | MIT OR Unlicense | Inside arc_os (linked or installed) | Image decoding for the image viewer (M3-08) |
| spleen | 2.2.0 | BSD-2-Clause | Inside arc_os (linked or installed) | Bitmap console fonts; ships ready-made PSF2 (.psfu) files (M3-05) |
| linux-uapi-ref | 6.12 | GPL-2.0 WITH Linux-syscall-note | Build host only | Reference only for syscall numbers and struct layouts (ADR-0001). Never #include in arc_os code. |
| ext2-doc | 2019 | GFDL-1.2-or-later | Build host only | The Second Extended File System layout spec by Dave Poirier (M1-13, M1-15) |
| tiny-chat-model | smollm2-135m | Apache-2.0 | Tests only (CI and QEMU) | Tiny GGUF chat model for QEMU and CI tests (M2) |
| tiny-embed-model | nomic-embed- | Apache-2.0 | Tests only (CI and QEMU) | Tiny GGUF embedder for retrieval tests (M3) |

## Notes

- **linux-uapi-ref** is reference material for syscall numbers and struct layouts (ADR-0001). Never
  `#include` it in arc_os code: arc_os writes its own headers, and `tests/test_abi_layout.c` compares
  them with the host's Linux headers.
- **e2fsprogs**: the `mke2fs` and `e2fsck` programs are GPL-2.0. Shipping them as separate programs with their
  source on the key is fine. Linking them into MIT arc_os code is not.
- **zstd** is dual-licensed. arc_os uses it under BSD-3-Clause.
- **Test models** are Apache-2.0 and used only by tests. The models users run are chosen in M2-06 and get
  their own entries.
