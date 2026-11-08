# arc_os — CMake toolchain file for freestanding x86_64 cross-compilation

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Cross-compiler toolchain paths.
# ARC_CROSS_PREFIX (cache variable, else environment variable) is the path prefix of the
# x86_64-elf tools, e.g. /opt/cross/bin/x86_64-elf-. Default: $HOME/opt/cross/bin/x86_64-elf-,
# where tools/dev-setup.sh installs the pinned toolchain.
if(NOT DEFINED ARC_CROSS_PREFIX)
    if(DEFINED ENV{ARC_CROSS_PREFIX})
        set(ARC_CROSS_PREFIX "$ENV{ARC_CROSS_PREFIX}")
    else()
        set(ARC_CROSS_PREFIX "$ENV{HOME}/opt/cross/bin/x86_64-elf-")
    endif()
endif()
set(ARC_CROSS_PREFIX "${ARC_CROSS_PREFIX}" CACHE STRING "Path prefix of the x86_64-elf cross tools")
# CMake re-reads the toolchain file in try_compile projects; pass the prefix through.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARC_CROSS_PREFIX)
set(CROSS_PREFIX "${ARC_CROSS_PREFIX}")

set(CMAKE_C_COMPILER "${CROSS_PREFIX}gcc")
set(CMAKE_ASM_NASM_COMPILER nasm)
set(CMAKE_LINKER "${CROSS_PREFIX}ld")
set(CMAKE_AR "${CROSS_PREFIX}ar")
set(CMAKE_OBJCOPY "${CROSS_PREFIX}objcopy")
set(CMAKE_RANLIB "${CROSS_PREFIX}ranlib")

# Freestanding C flags
set(CMAKE_C_FLAGS_INIT
    "-ffreestanding -nostdlib -nostdinc -mno-red-zone -mcmodel=kernel \
     -fno-pic -fno-pie -fstack-protector-strong -Wall -Wextra -Werror -std=c11")

# Skip compiler checks — freestanding environment has no libc
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_NASM_COMPILER_WORKS TRUE)

# Never search host paths for libraries or headers
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)
