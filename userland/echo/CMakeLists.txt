# arc_os — User-space echo binary (cross-compiled, freestanding)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(echo echo.c)

# Freestanding user-space flags (no -mcmodel=kernel, no kernel includes)
target_compile_options(echo PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

# Use freestanding headers from kernel/include (stdint.h, etc.)
target_include_directories(echo PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

# Linker flags: freestanding, use our linker script
set_target_properties(echo PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_CURRENT_SOURCE_DIR}/echo.ld"
)

# Copy echo binary to build root for ISO creation
add_custom_command(TARGET echo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:echo> ${CMAKE_BINARY_DIR}/echo
    COMMENT "Copying echo binary to build directory"
)
