# arc_os — Minimal C library (libarc.a)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

# Determine GCC's freestanding include path (stdint.h, stddef.h, stdarg.h, etc.)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE GCC_FREESTANDING_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(LIBC_SOURCES
    src/crt0.c
    src/string.c
    src/stdlib.c
    src/malloc.c
    src/printf.c
    src/stdio.c
    src/unistd.c
    src/dirent.c
    src/signal.c
    src/stat.c
    src/wait.c
)

add_library(arc STATIC ${LIBC_SOURCES})

# Public: libc headers + GCC freestanding headers (consumers get these)
target_include_directories(arc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GCC_FREESTANDING_INCLUDE}
)

target_compile_options(arc PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)
