# arc_os — User-space shell binary (cross-compiled, freestanding)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(shell shell.c)

# Freestanding user-space flags (no -mcmodel=kernel, no kernel includes)
target_compile_options(shell PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

# Use freestanding headers from kernel/include (stdint.h, etc.)
target_include_directories(shell PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

# Linker flags: freestanding, use our linker script
set_target_properties(shell PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_CURRENT_SOURCE_DIR}/shell.ld"
)

# Copy shell binary to build root for ISO creation
add_custom_command(TARGET shell POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:shell> ${CMAKE_BINARY_DIR}/shell
    COMMENT "Copying shell binary to build directory"
)
