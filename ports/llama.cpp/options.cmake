# llama.cpp b12198 build options for arc_os (M2-03): a CMake initial cache (cmake -C), shared by
# the port (build.sh, musl, static) and the test-only reference build (build-ref.sh, host GCC),
# so both compile the same code. Each option's reason is in PORT.md.

# One CPU backend for the arc_os floor (x86-64-v3), compiled in, no runtime variants.
set(GGML_NATIVE OFF CACHE BOOL "")
set(GGML_SSE42 ON CACHE BOOL "")
set(GGML_AVX ON CACHE BOOL "")
set(GGML_AVX2 ON CACHE BOOL "")
set(GGML_FMA ON CACHE BOOL "")
set(GGML_F16C ON CACHE BOOL "")
set(GGML_BMI2 ON CACHE BOOL "")
set(GGML_AVX_VNNI OFF CACHE BOOL "")
set(GGML_AVX512 OFF CACHE BOOL "")
set(GGML_BACKEND_DL OFF CACHE BOOL "")
set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "")
set(GGML_OPENMP OFF CACHE BOOL "")
set(GGML_OPENMP_FETCH OFF CACHE BOOL "")
set(GGML_BLAS OFF CACHE BOOL "")
set(GGML_CCACHE OFF CACHE BOOL "")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")

# Nothing that reaches the network, at build time or at run time.
set(GGML_RPC OFF CACHE BOOL "")
set(LLAMA_OPENSSL OFF CACHE BOOL "")
set(LLAMA_BUILD_BORINGSSL OFF CACHE BOOL "")
set(LLAMA_BUILD_LIBRESSL OFF CACHE BOOL "")
set(LLAMA_LLGUIDANCE OFF CACHE BOOL "")
set(LLAMA_CONNECT OFF CACHE BOOL "")
set(LLAMA_BUILD_UI OFF CACHE BOOL "")
set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "")
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "")

# Libraries plus the command-line tools the lane tests use; no tests, server, app or examples
# beyond what the tools need (llama-embedding lives under examples/).
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "")
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "")
set(LLAMA_BUILD_APP OFF CACHE BOOL "")
set(LLAMA_SUBPROCESS OFF CACHE BOOL "")
set(LLAMA_BUILD_TOOLS ON CACHE BOOL "")
set(LLAMA_BUILD_EXAMPLES ON CACHE BOOL "")
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
