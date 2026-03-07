# arc_os — User-space init binary (cross-compiled, freestanding)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(init init.c)

# Freestanding user-space flags (no -mcmodel=kernel, no kernel includes)
target_compile_options(init PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

# Use freestanding headers from kernel/include (stdint.h, etc.)
target_include_directories(init PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

# Linker flags: freestanding, use our linker script
set_target_properties(init PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_CURRENT_SOURCE_DIR}/init.ld"
)

# Copy init binary to build root for ISO creation
add_custom_command(TARGET init POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:init> ${CMAKE_BINARY_DIR}/init
    COMMENT "Copying init binary to build directory"
)
