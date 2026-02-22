# arc_os — CMake toolchain file for freestanding x86_64 cross-compilation

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Cross-compiler toolchain paths
set(CROSS_PREFIX "$ENV{HOME}/opt/cross/bin/x86_64-elf-")

set(CMAKE_C_COMPILER "${CROSS_PREFIX}gcc")
set(CMAKE_ASM_NASM_COMPILER nasm)
set(CMAKE_LINKER "${CROSS_PREFIX}ld")
set(CMAKE_AR "${CROSS_PREFIX}ar")
set(CMAKE_OBJCOPY "${CROSS_PREFIX}objcopy")
set(CMAKE_RANLIB "${CROSS_PREFIX}ranlib")

# Freestanding C flags
set(CMAKE_C_FLAGS_INIT
    "-ffreestanding -nostdlib -nostdinc -mno-red-zone -mcmodel=kernel \
     -fno-pic -fno-pie -fno-stack-protector -Wall -Wextra -Werror -std=c11")

# Skip compiler checks — freestanding environment has no libc
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_NASM_COMPILER_WORKS TRUE)

# Never search host paths for libraries or headers
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)
