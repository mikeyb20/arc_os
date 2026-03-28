# arc_os — User-space echo binary (linked against libarc)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(echo echo.c)

target_compile_options(echo PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

target_link_libraries(echo PRIVATE arc)

set_target_properties(echo PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
)

add_custom_command(TARGET echo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:echo> ${CMAKE_BINARY_DIR}/echo
    COMMENT "Copying echo binary to build directory"
)
