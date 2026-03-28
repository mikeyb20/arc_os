# arc_os — login program (linked against libarc)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(login login.c)

target_compile_options(login PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)

target_link_libraries(login PRIVATE arc)

set_target_properties(login PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
)

add_custom_command(TARGET login POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:login> ${CMAKE_BINARY_DIR}/login
    COMMENT "Copying login binary to build directory"
)
