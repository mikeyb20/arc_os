# arc_os — User-space init binary (linked against libarc)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(init init.c)

target_compile_options(init PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

target_link_libraries(init PRIVATE arc)

set_target_properties(init PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
)

add_custom_command(TARGET init POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:init> ${CMAKE_BINARY_DIR}/init
    COMMENT "Copying init binary to build directory"
)
