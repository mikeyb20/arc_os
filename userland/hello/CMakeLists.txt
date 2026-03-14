# arc_os — User-space hello binary (cross-compiled, freestanding)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(hello hello.c)

# Freestanding user-space flags (no -mcmodel=kernel, no kernel includes)
target_compile_options(hello PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

# Use freestanding headers from kernel/include (stdint.h, etc.)
target_include_directories(hello PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

# Linker flags: freestanding, use our linker script
set_target_properties(hello PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_CURRENT_SOURCE_DIR}/hello.ld"
)

# Copy hello binary to build root for ISO creation
add_custom_command(TARGET hello POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hello> ${CMAKE_BINARY_DIR}/hello
    COMMENT "Copying hello binary to build directory"
)
