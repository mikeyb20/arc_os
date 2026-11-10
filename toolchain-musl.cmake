# arc_os — CMake toolchain file for user space built with musl (lane L, M2-01).
#
# Static x86_64-linux-musl programs for the arc_os CPU floor (x86-64-v3). The same binaries
# run on Linux (lane tests) and on arc_os (Linux syscall ABI, ADR-0001). Used by stack/:
#   cmake -S stack -B build-lane/stack -G Ninja -DCMAKE_TOOLCHAIN_FILE=../toolchain-musl.cmake
# (tools/lane/build.sh does this).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/musl-prefix.cmake")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARC_MUSL_PREFIX)

set(CMAKE_C_COMPILER "${ARC_MUSL_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${ARC_MUSL_PREFIX}g++")
set(CMAKE_AR "${ARC_MUSL_PREFIX}ar")
set(CMAKE_RANLIB "${ARC_MUSL_PREFIX}ranlib")
set(CMAKE_STRIP "${ARC_MUSL_PREFIX}strip")

set(CMAKE_C_FLAGS_INIT "-march=x86-64-v3")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64-v3")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Libraries and headers come from the musl sysroot and the ports (build-lane/ports/<name>),
# never from the host.
get_filename_component(_arc_root "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(CMAKE_FIND_ROOT_PATH "${_arc_root}/build-lane/ports")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
