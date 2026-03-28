# arc_os — User-space shell binary (linked against libarc)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(shell shell.c)

target_compile_options(shell PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

target_link_libraries(shell PRIVATE arc)

set_target_properties(shell PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
)

add_custom_command(TARGET shell POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:shell> ${CMAKE_BINARY_DIR}/shell
    COMMENT "Copying shell binary to build directory"
)
