# arc_os — Test-only user programs (M0-02). Shipped only on test ISOs.

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

foreach(prog testfail)
    add_executable(${prog} ${prog}.c)
    target_compile_options(${prog} PRIVATE
        -ffreestanding -nostdlib -nostdinc -static
        -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
        -Wall -Wextra -Werror -std=c11
    )
    target_link_libraries(${prog} PRIVATE arc)
    set_target_properties(${prog} PROPERTIES
        LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
    )
    add_custom_command(TARGET ${prog} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${prog}> ${CMAKE_BINARY_DIR}/${prog}
        COMMENT "Copying ${prog} binary to build directory"
    )
endforeach()
