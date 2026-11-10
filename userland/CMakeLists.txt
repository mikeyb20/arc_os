# arc_os — User-space programs (linked against libarc)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

# arc_user_program(<target> <source> [OUTPUT <name>])
# A static libarc program linked with libc/user.ld, copied to the top of the build directory
# as <name> (default <target>), where the ISO and the QEMU tests pick it up. OUTPUT lets a
# program keep its binary name when that name is already a CMake target (echo_cmd -> echo).
function(arc_user_program target source)
    cmake_parse_arguments(ARG "" "OUTPUT" "" ${ARGN})
    if(NOT ARG_OUTPUT)
        set(ARG_OUTPUT ${target})
    endif()
    add_executable(${target} ${source})
    target_compile_options(${target} PRIVATE
        -ffreestanding -nostdlib -nostdinc -static
        -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
        -Wall -Wextra -Werror -std=c11
    )
    target_link_libraries(${target} PRIVATE arc)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${ARG_OUTPUT}
        LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
    )
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target}> ${CMAKE_BINARY_DIR}/${ARG_OUTPUT}
        COMMENT "Copying ${ARG_OUTPUT} to build directory"
    )
endfunction()

add_subdirectory(init)
add_subdirectory(login)
add_subdirectory(hello)
add_subdirectory(echo)
add_subdirectory(shell)
add_subdirectory(coreutils)
add_subdirectory(testprogs)
