# M2-03 — Port llama.cpp (CPU, static, x86-64-v3)

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | D | L | M2-01 | M2-04 |

**Goal.** llama.cpp's libraries and test tools built statically with the musl toolchain for the CPU floor
(x86-64-v3), whose greedy output matches a reference build on Linux, tested inside the syscall inventory.

**Read first.**
- `docs/plans/lane-l.md`, `.claude/skills/port-package/SKILL.md`
- `third_party/SOURCES.lock` entries `llama.cpp`, `tiny-chat-model`, `tiny-embed-model`
- `tools/inventory/run-in-alpine.sh` (the M0-04 llama.cpp configure line) and Notes of M0-04 and M2-01

**Build.**
1. `ports/llama.cpp/`: `PORT.md` and `build.sh`. CMake options: the M0-04 set (`GGML_NATIVE=OFF`, AVX, AVX2, FMA,
   F16C, BMI2 on, `GGML_OPENMP=OFF`, `LLAMA_OPENSSL=OFF`, `BUILD_SHARED_LIBS=OFF`, tests, server and app off) plus
   `GGML_BACKEND_DL=OFF`, `GGML_CPU_ALL_VARIANTS=OFF` and every network option off (whatever this tag names them;
   list each in `PORT.md` with its reason). Install `libllama.a`, the `libggml*.a` libraries, `llama.h`, the
   `ggml*.h` headers it needs, and the tools `llama-completion`, `llama-embedding` and `llama-tokenize` into
   `build-lane/ports/llama.cpp/`.
2. Reference build, test-only: the same source and options built with the host's own compiler (Ubuntu GCC, glibc,
   not static) into `build-lane/llama-ref/`. It never ships; it only answers "did our toolchain change the math?".
3. Lane tests in `stack/tests/llama/` (drivers in shell or Python, untraced; our binaries through
   `tools/lane/run.sh`), on the archived tiny models:
   - `lane.llama_greedy`: `llama-completion -t 4 -n 32 --temp 0 --seed 1 -no-cnv -p "Water boils at"` from both
     builds; stdout text identical.
   - `lane.llama_embed`: `llama-embedding` on two fixed texts from both builds; cosine similarity of each pair
     ≥ 0.9999.
   - `lane.llama_tokenize`: `llama-tokenize` token IDs from both builds identical for a UTF-8 string with
     `°`, `µ` and `±`.
   - `lane.llama_nommap`: the greedy test again with `--no-mmap` (the anonymous-memory load path that M2-06
     compares with `mmap`); same text.
4. Budgets: `llama_completion_bytes`; `llama_tiny_rss_kib` (peak RSS of the greedy run, `ru_maxrss` from the
   driver).

**Done when.**
- [ ] The port builds offline; all binaries are static.
- [ ] The four lane tests pass and the syscall check passes in `lane.yml`.

**Cost.** No kernel hot path. One static binary per tool; the budget lines above are the ADR-0019 record. The
model is memory-mapped read-only (`MAP_SHARED|MAP_POPULATE` in the M0-04 inventory), so on arc_os it lives in the
page cache, not in each process's heap.

**Watch out.**
- glibc and musl `libm` can differ in the last bit of `expf`/`tanhf`, which can flip a late greedy token. If the
  32-token test differs, find the first differing step and the function responsible before changing anything; any
  looser check needs Michael's OK and goes in this spec.
- One binary for the CPU floor: no AVX-VNNI or AVX-512 paths (the low tier has neither). Faster variants for the
  laptop are a later decision, not this block.
- llama.cpp reads `/proc/cpuinfo`, `/dev/tty`, `/etc/llama.cpp/config.ini` and calls `sysinfo`; all are in the
  inventory. Anything new (for example `/sys/devices/system/cpu/…` for core types) needs a row and an owner block.
- `llama-tokenize` might not exist under that name at this tag; use whichever upstream tool prints token IDs and
  say so in `PORT.md`.

**Notes.**
-
