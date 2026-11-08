# arc_os — Test-only user programs (M0-02, M0-05). Shipped only on test ISOs.

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

foreach(prog testfail abi_check)
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

# M0-06: tests/target/start_basic.c (System V process start) built three ways:
# with libarc (start_arc), and with the host's stock musl as static ET_EXEC
# (start_musl) and static-PIE (start_musl_pie). Needs musl-tools (dev-setup.sh).
add_executable(start_arc ${CMAKE_SOURCE_DIR}/tests/target/start_basic.c)
target_compile_options(start_arc PRIVATE
    -ffreestanding -nostdlib -nostdinc -static
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
    -Wall -Wextra -Werror -std=c11
)
target_link_libraries(start_arc PRIVATE arc)
set_target_properties(start_arc PROPERTIES
    LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
)
add_custom_command(TARGET start_arc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:start_arc> ${CMAKE_BINARY_DIR}/start_arc
)

find_program(MUSL_GCC musl-gcc)
if(MUSL_GCC)
    set(_start_src ${CMAKE_SOURCE_DIR}/tests/target/start_basic.c)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/start_musl
        COMMAND ${MUSL_GCC} -static -O2 -Wall -Wextra -Werror -std=c11
                -o ${CMAKE_BINARY_DIR}/start_musl ${_start_src}
        DEPENDS ${_start_src}
        COMMENT "musl-gcc -static start_musl")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/start_musl_pie
        COMMAND ${MUSL_GCC} -static-pie -fPIE -O2 -Wall -Wextra -Werror -std=c11
                -o ${CMAKE_BINARY_DIR}/start_musl_pie ${_start_src}
        DEPENDS ${_start_src}
        COMMENT "musl-gcc -static-pie start_musl_pie")
    add_custom_target(musl_testprogs ALL
        DEPENDS ${CMAKE_BINARY_DIR}/start_musl ${CMAKE_BINARY_DIR}/start_musl_pie)
else()
    message(WARNING "musl-gcc not found (apt install musl-tools): qemu.musl will fail")
endif()
