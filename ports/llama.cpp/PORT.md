# llama.cpp — CPU inference for the model service

| | |
|---|---|
| What | llama.cpp b12198: `libllama.a`, `libggml.a`, `libggml-base.a`, `libggml-cpu.a`, `llama.h` and the ggml headers it includes, and the tools `llama-completion`, `llama-embedding`, `llama-tokenize`; static, musl, x86-64-v3 |
| Source | `third_party/SOURCES.lock` entry `llama.cpp` (SHA-256 `253cb3ef…b1ae`, archived release tarball) |
| Licence | MIT (ggml and llama.cpp). The tools also link vendored cpp-httplib (MIT), nlohmann/json (MIT) and sheredom `subprocess.h` (Unlicense, public domain); `libllama.a` and `libggml*.a` contain none of them |
| Why | ADR-0012 (`arc-llm` runs GGUF models with llama.cpp, CPU only), knowledge-os.md §3.4 |
| Used by | `arc-llm` (M2-04) links `libllama.a` + `libggml*.a`; the three tools are for the lane tests and debugging |
| Built by | `build.sh <prefix>`, offline, from `tools/lane/build.sh` into `build-lane/ports/llama.cpp/`; options in `options.cmake` |
| Reference | `build-ref.sh <dir>` (test-only, never shipped): the same options with the host's GCC 14 and glibc, in `build-lane/ref/llama.cpp/` |

## Build options (`options.cmake`, used by both builds)

| Option | Why |
|---|---|
| `GGML_NATIVE=OFF`; `GGML_SSE42`, `AVX`, `AVX2`, `FMA`, `F16C`, `BMI2` on | The arc_os CPU floor, x86-64-v3, fixed at build time; never the build machine's CPU |
| `GGML_AVX_VNNI=OFF`, `GGML_AVX512=OFF` | The LOW tier has neither. Faster variants for the laptop are a later decision |
| `GGML_BACKEND_DL=OFF`, `GGML_CPU_ALL_VARIANTS=OFF`, `BUILD_SHARED_LIBS=OFF` | One CPU backend linked in; arc_os has no dynamic loading (static ELF only, ADR-0002) |
| `GGML_OPENMP=OFF`, `GGML_OPENMP_FETCH=OFF` | ggml's own thread pool on pthreads; no OpenMP runtime to port, nothing fetched |
| `GGML_BLAS=OFF`, `GGML_CCACHE=OFF` | No outside BLAS; builds don't depend on a host cache |
| `GGML_RPC=OFF` | RPC backend = network sockets to other machines |
| `LLAMA_OPENSSL=OFF`, `LLAMA_BUILD_BORINGSSL=OFF`, `LLAMA_BUILD_LIBRESSL=OFF` | No TLS; the last two would download BoringSSL or LibreSSL at configure time |
| `LLAMA_LLGUIDANCE=OFF`, `LLAMA_CONNECT=OFF`, `LLAMA_BUILD_UI=OFF`, `LLAMA_USE_PREBUILT_UI=OFF` | Each downloads something at build time (a Rust crate, a prebuilt binary, web UI assets) |
| `FETCHCONTENT_FULLY_DISCONNECTED=ON` | Belt and braces: any FetchContent left in the tree fails instead of downloading |
| `LLAMA_BUILD_TESTS`, `LLAMA_BUILD_SERVER`, `LLAMA_BUILD_APP`, `LLAMA_SUBPROCESS` off | Not needed; the server would also pull in the web UI and subprocess spawning |
| `LLAMA_BUILD_TOOLS`, `LLAMA_BUILD_EXAMPLES` on (only three targets built) | `llama-completion` and `llama-tokenize` are tools, `llama-embedding` is an example at this tag |

`build.sh` builds only the targets `llama`, `llama-completion`, `llama-embedding` and `llama-tokenize`, so the other
tools and examples are configured but never compiled. Compiler flags come from `toolchain-musl.cmake`
(`-march=x86-64-v3`, `-static`).

## Patches

- `0001-download-path-max-from-limits-h.patch`: `common/download.cpp` includes `<linux/limits.h>`; the musl
  toolchain has no Linux kernel headers (M2-01), and `<limits.h>` serves. Upstream builds on Alpine only because the
  `linux-headers` package is installed there. Only the tools link `common/`; `libllama.a` is unaffected.

## What it asks of the kernel

- The model is mapped `PROT_READ`, `MAP_SHARED|MAP_POPULATE` (default), or read into anonymous memory with
  `-lm none` (this tag's name for the old `--no-mmap`; `lane.llama_nommap` checks the strace shows no mapping of
  the file).
- `/proc/cpuinfo`, `/dev/tty`, `/etc/llama.cpp/config.ini` (optional) and `sysinfo`: in the inventory since M0-04.
- Without `-t`, `common/` counts physical cores through `/sys/devices/system/cpu/cpu<n>/topology/thread_siblings`.
  arc_os has no `/sys`; the code falls back when the file is missing, and every caller of ours passes the thread
  count (the lane tests use `-t 4`; `arc-llm` sets it, M2-04), so no inventory row.

## Known gaps

- The tools link `common/`, which carries cpp-httplib for `-hf` / `--model-url` downloads (plain HTTP only:
  OpenSSL is off). It runs only if given a URL; `arc-llm` links `libllama` and `libggml*` only, never `common/`.
- Upstream's GCC 13 and GCC 14 builds differ in the last bits of float results (embeddings at cosine 0.99988);
  that is why the reference uses GCC 14 (our toolchain is GCC 14.1). With it, greedy text, token IDs and embeddings
  are bit-identical to the musl build: musl's libm made no difference here (checked by preloading musl's float
  functions into the glibc reference).
