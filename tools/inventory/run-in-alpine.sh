#!/bin/sh
# Syscall inventory (M0-04), run inside an Alpine (musl) container by
# .github/workflows/inventory.yml:
#
#   docker run --cap-add SYS_PTRACE -v "$PWD:/work" -w /work alpine:3.20 sh tools/inventory/run-in-alpine.sh
#
# Builds the target programs, the SQLite shell (FTS5), the zstd CLI and llama.cpp
# statically against musl from the archived sources (tools/fetch-archived.sh ran
# first, on the host), then runs each under strace. Output in build/inventory/:
#   bin/          the static binaries
#   logs/*.strace one strace log per run;  logs/*.out  its stdout/stderr
#   status.txt    one line per build and run: "<step> ok|FAILED [detail]"
# A failed build or run is recorded, not fatal, so one broken port still leaves
# the rest of the inventory (and the failure is early warning for M2).
set -u
OUT=/work/build/inventory
SRC=/work/third_party/src
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/logs" "$OUT/tmp"
STATUS="$OUT/status.txt"
: > "$STATUS"

step() {   # step <name> <command...>: run, log output, record ok/FAILED
    name=$1; shift
    if "$@" > "$OUT/logs/build-$name.log" 2>&1; then
        echo "build-$name ok" >> "$STATUS"
    else
        echo "build-$name FAILED (exit $?, see logs/build-$name.log)" >> "$STATUS"
        tail -20 "$OUT/logs/build-$name.log"
    fi
}

apk add --no-cache build-base cmake samurai linux-headers strace file > /dev/null
gcc --version | head -1 >> "$STATUS"

step libc_basic    gcc -static -O2 -o "$OUT/bin/libc_basic" /work/tests/target/libc_basic.c
step pthread_basic gcc -static -O2 -pthread -o "$OUT/bin/pthread_basic" /work/tests/target/pthread_basic.c
step cxx_basic     g++ -static -O2 -std=c++17 -pthread -o "$OUT/bin/cxx_basic" /work/tests/target/cxx_basic.cpp
step sqlite3       gcc -static -O2 -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1 \
                       -o "$OUT/bin/sqlite3" "$SRC/sqlite/shell.c" "$SRC/sqlite/sqlite3.c" -lm
step zstd          sh -c "make -C '$SRC/zstd/programs' -j4 zstd LDFLAGS=-static && cp '$SRC/zstd/programs/zstd' '$OUT/bin/'"
# llama.cpp for x86-64-v3 (the arc_os CPU floor), no OpenMP (its own thread
# pool on pthreads), no TLS/HTTP. M2-03 owns the real build options.
step llama sh -c "cmake -S '$SRC/llama.cpp' -B '$OUT/llama' -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON \
        -DGGML_F16C=ON -DGGML_BMI2=ON -DGGML_OPENMP=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_TESTS=OFF \
        -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF -DCMAKE_EXE_LINKER_FLAGS=-static \
    && cmake --build '$OUT/llama' -j4 --target llama-completion llama-embedding \
    && cp '$OUT/llama/bin/llama-completion' '$OUT/llama/bin/llama-embedding' '$OUT/bin/'"

for b in "$OUT"/bin/*; do
    if file "$b" 2>/dev/null | grep -q dynamic; then echo "static-check $(basename "$b") FAILED (dynamic)" >> "$STATUS"; fi
done

run() {    # run <name> <command...>: strace one run, record its exit status
    name=$1; shift
    if [ ! -x "$1" ]; then
        echo "run-$name FAILED (not built)" >> "$STATUS"
        return
    fi
    strace -f -qq -s 200 -o "$OUT/logs/$name.strace" "$@" > "$OUT/logs/$name.out" 2>&1 < /dev/null
    rc=$?
    if [ "$rc" = 0 ]; then echo "run-$name ok" >> "$STATUS"; else echo "run-$name FAILED (exit $rc)" >> "$STATUS"; fi
}

cd "$OUT/tmp"
run libc_basic    "$OUT/bin/libc_basic"
run pthread_basic "$OUT/bin/pthread_basic"
run cxx_basic     "$OUT/bin/cxx_basic"
cat > fts.sql <<'SQL'
CREATE VIRTUAL TABLE docs USING fts5(title, body);
INSERT INTO docs VALUES ('Hypothermia', 'Body temperature below 35 C; warm the person slowly.');
INSERT INTO docs VALUES ('Water', 'Boil water for one minute to make it safe to drink.');
SELECT title FROM docs WHERE docs MATCH 'warm' ORDER BY rank;
SQL
run sqlite3       "$OUT/bin/sqlite3" -bail -cmd ".read fts.sql" fts.db ".quit"
head -c 3000000 "$OUT/bin/sqlite3" > sample.bin
run zstd-compress   "$OUT/bin/zstd" -q -f -3 -T2 sample.bin -o sample.bin.zst
run zstd-decompress "$OUT/bin/zstd" -q -f -d sample.bin.zst -o sample.out
cmp -s sample.bin sample.out && echo "zstd-roundtrip ok" >> "$STATUS" || echo "zstd-roundtrip FAILED" >> "$STATUS"
run llama-completion "$OUT/bin/llama-completion" -m "$SRC/tiny-chat-model/SmolLM2-135M-Instruct-Q8_0.gguf" \
    -t 4 -n 16 --temp 0 -no-cnv -p "Water boils at"
run llama-embedding "$OUT/bin/llama-embedding" -m "$SRC/tiny-embed-model/nomic-embed-text-v1.5.Q8_0.gguf" \
    -t 4 -p "search_query: how to purify water"
cat "$STATUS"
