# arc_os — Coreutils (linked against libarc)

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

# Map source file → binary name (where they differ)
set(COREUTILS_PROGRAMS
    cat
    wc
    head
    tail
    touch
    cp
    mv
    ls
    grep
    ps
    uname
    rm
)

# Programs with _cmd suffix (to avoid CMake target name conflicts)
set(COREUTILS_RENAMED
    echo_cmd:echo
    mkdir_cmd:mkdir
    stat_cmd:stat
    chmod_cmd:chmod
    chown_cmd:chown
    kill_cmd:kill
    free_cmd:free
)

# Build simple programs (source name == binary name)
foreach(prog ${COREUTILS_PROGRAMS})
    add_executable(${prog}_bin ${prog}.c)
    set_target_properties(${prog}_bin PROPERTIES OUTPUT_NAME ${prog})

    target_compile_options(${prog}_bin PRIVATE
        -ffreestanding -nostdlib -nostdinc -static
        -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
        -Wall -Wextra -Werror -std=c11
    )

    target_link_libraries(${prog}_bin PRIVATE arc)

    set_target_properties(${prog}_bin PROPERTIES
        LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
    )

    add_custom_command(TARGET ${prog}_bin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${prog}_bin> ${CMAKE_BINARY_DIR}/${prog}
        COMMENT "Copying ${prog} to build directory"
    )
endforeach()

# Build renamed programs (source_cmd.c → binary_name)
foreach(entry ${COREUTILS_RENAMED})
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 src_name)
    list(GET parts 1 bin_name)

    add_executable(${src_name} ${src_name}.c)
    set_target_properties(${src_name} PROPERTIES OUTPUT_NAME ${bin_name})

    target_compile_options(${src_name} PRIVATE
        -ffreestanding -nostdlib -nostdinc -static
        -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
        -Wall -Wextra -Werror -std=c11
    )

    target_link_libraries(${src_name} PRIVATE arc)

    set_target_properties(${src_name} PROPERTIES
        LINK_FLAGS "-nostdlib -static -T ${CMAKE_SOURCE_DIR}/libc/user.ld"
    )

    add_custom_command(TARGET ${src_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${src_name}> ${CMAKE_BINARY_DIR}/${bin_name}
        COMMENT "Copying ${bin_name} to build directory"
    )
endforeach()
