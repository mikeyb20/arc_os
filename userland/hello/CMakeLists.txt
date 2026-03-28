# arc_os — User-space hello binary (linked against libarc)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(hello hello.c)

target_compile_options(hello PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

target_link_libraries(hello PRIVATE arc)

set_target_properties(hello PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
)

add_custom_command(TARGET hello POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hello> ${CMAKE_BINARY_DIR}/hello
    COMMENT "Copying hello binary to build directory"
)
