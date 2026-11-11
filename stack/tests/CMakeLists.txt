# Lane tests (label `lane`). M2-01: the M0-04 target programs, built with the pinned
# musl toolchain, pass on Linux inside the syscall inventory. M2-02, M2-03: the ports (ports/).
# M2-04: arcjson and arc-llm. M3-02: arcpack. M3-06a: search. M5-02: arcutil and arcpkg.

set(ARC_TARGET_DIR "${ARC_ROOT}/tests/target")

add_executable(libc_basic "${ARC_TARGET_DIR}/libc_basic.c")
add_executable(pthread_basic "${ARC_TARGET_DIR}/pthread_basic.c")
target_link_libraries(pthread_basic PRIVATE pthread)
add_executable(cxx_basic "${ARC_TARGET_DIR}/cxx_basic.cpp")
target_link_libraries(cxx_basic PRIVATE pthread)

# libc_basic execs `true`; under strace -f the child is traced too, so give it a static
# musl one instead of the host's dynamic /bin/true (which would show the host's loader).
add_executable(lane_true lane_true.c)
lane_test(NAME libc_basic COMMAND $<TARGET_FILE:libc_basic> ENVIRONMENT ARC_TRUE=$<TARGET_FILE:lane_true>)
lane_test(NAME pthread_basic COMMAND $<TARGET_FILE:pthread_basic>)
lane_test(NAME cxx_basic COMMAND $<TARGET_FILE:cxx_basic>)

# lane_rss: runs a program and reports its own peak RSS, for whole-program RSS budgets (M3-06a).
add_executable(lane_rss lane_rss.c)

add_subdirectory(ports)

# The llama.cpp tools and the archived tiny models (tools/lane/build.sh fetches them), for the
# llama.cpp port tests (M2-03) and the arc-llm tests (M2-04).
get_filename_component(ARC_LLAMA_OURS "${ARC_LLAMA_COMPLETION}" DIRECTORY)
set(ARC_TINY_CHAT "${ARC_ROOT}/third_party/src/tiny-chat-model/SmolLM2-135M-Instruct-Q8_0.gguf")
set(ARC_TINY_EMBED "${ARC_ROOT}/third_party/src/tiny-embed-model/nomic-embed-text-v1.5.Q8_0.gguf")
add_subdirectory(llama)
add_subdirectory(arcjson)
add_subdirectory(arc-llm)
add_subdirectory(arcpack)
add_subdirectory(search)
add_subdirectory(arcutil)
add_subdirectory(arcpkg)

# Sizes for stack/budgets.txt (ADR-0019): loaded ELF bytes, as kernel_image_bytes.
lane_driver_test(NAME sizes COMMAND python3 "${ARC_ROOT}/tools/lane/sizes.py"
    "${ARC_LANE_BUDGET_DIR}/sizes.budget.log" cxx_basic_bytes=$<TARGET_FILE:cxx_basic>
    zstd_cli_bytes=${ARC_ZSTD_CLI} sqlite3_shell_bytes=${ARC_SQLITE_SHELL}
    llama_completion_bytes=${ARC_LLAMA_COMPLETION} arc_llm_bytes=$<TARGET_FILE:arc-llm>
    arcpkg_bytes=$<TARGET_FILE:arcpkg>)
