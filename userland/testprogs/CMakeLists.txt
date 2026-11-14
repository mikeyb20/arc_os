# arc_os — Test-only user programs (M0-02, M0-05, M0-10, M1-01, M1-05, M1-07, M1-11, M1-12). Shipped only on test ISOs.

foreach(prog testfail abi_check forkreap schedtest)
    arc_user_program(${prog} ${prog}.c)
endforeach()

# M0-06: tests/target/start_basic.c (System V process start) built three ways: with libarc
# (start_arc), and with musl as static ET_EXEC (start_musl) and static-PIE (start_musl_pie).
arc_user_program(start_arc ${CMAKE_SOURCE_DIR}/tests/target/start_basic.c)

# musl programs use the pinned toolchain that stack/ uses (tools/dev-setup.sh musl;
# cmake/musl-prefix.cmake). Its libc.a is not built as PIC, so it can't
# link static-PIE: start_musl_pie alone still uses the host's musl-gcc (musl-tools).
include(${CMAKE_SOURCE_DIR}/cmake/musl-prefix.cmake)
set(_start_src ${CMAKE_SOURCE_DIR}/tests/target/start_basic.c)
set(_musl_flags -static -O2 -Wall -Wextra -Werror -std=c11)
set(_musl_outputs)

if(EXISTS "${ARC_MUSL_PREFIX}gcc")
    set(MUSL_CC "${ARC_MUSL_PREFIX}gcc")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/start_musl
        COMMAND ${MUSL_CC} ${_musl_flags} -o ${CMAKE_BINARY_DIR}/start_musl ${_start_src}
        DEPENDS ${_start_src}
        COMMENT "musl -static start_musl")
    # M1-01: AVX2 state across preemption and signals, with musl's own
    # sigaction and __restore_rt (qemu.fpu)
    foreach(prog sig_fpu fpu_avx)
        string(TOUPPER ${prog} _define)
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${prog}
            COMMAND ${MUSL_CC} ${_musl_flags} -D${_define}
                    -o ${CMAKE_BINARY_DIR}/${prog} ${CMAKE_CURRENT_SOURCE_DIR}/${prog}.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/fpu_test.S
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${prog}.c ${CMAKE_CURRENT_SOURCE_DIR}/fpu_test.S
            COMMENT "musl -static ${prog}")
    endforeach()
    # M1-07: address spaces and the mmap family under musl's malloc (qemu.mm)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/mmtest
        COMMAND ${MUSL_CC} -static -O2 -Wall -Wextra -Werror -std=gnu11
                -o ${CMAKE_BINARY_DIR}/mmtest ${CMAKE_CURRENT_SOURCE_DIR}/mmtest.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mmtest.c
        COMMENT "musl -static mmtest")
    # M1-11: files, directories, fds and mounts through the VFS as musl calls them (qemu.vfs)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/fstest
        COMMAND ${MUSL_CC} -static -O2 -Wall -Wextra -Werror -std=gnu11
                -o ${CMAKE_BINARY_DIR}/fstest ${CMAKE_CURRENT_SOURCE_DIR}/fstest.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fstest.c
        COMMENT "musl -static fstest")
    # M1-12: the page cache and file mmap (qemu.pagecache), and a 9 MiB
    # program exec maps from the cache
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/pctest
        COMMAND ${MUSL_CC} -static -O2 -Wall -Wextra -Werror -std=gnu11
                -o ${CMAKE_BINARY_DIR}/pctest ${CMAKE_CURRENT_SOURCE_DIR}/pctest.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pctest.c
        COMMENT "musl -static pctest")
    # M1-13: arc_os's ext2 partitions mounted at boot, read with checksums (qemu.ext2)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/ext2test
        COMMAND ${MUSL_CC} -static -O2 -Wall -Wextra -Werror -std=gnu11
                -o ${CMAKE_BINARY_DIR}/ext2test ${CMAKE_CURRENT_SOURCE_DIR}/ext2test.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ext2test.c
        COMMENT "musl -static ext2test")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/bigexec
        COMMAND ${MUSL_CC} -static -O2 -Wall -Wextra -Werror -std=gnu11
                -o ${CMAKE_BINARY_DIR}/bigexec ${CMAKE_CURRENT_SOURCE_DIR}/bigexec.c
                ${CMAKE_CURRENT_SOURCE_DIR}/bigexec_data.S
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bigexec.c ${CMAKE_CURRENT_SOURCE_DIR}/bigexec_data.S
        COMMENT "musl -static bigexec")
    list(APPEND _musl_outputs ${CMAKE_BINARY_DIR}/start_musl ${CMAKE_BINARY_DIR}/sig_fpu ${CMAKE_BINARY_DIR}/fpu_avx
         ${CMAKE_BINARY_DIR}/mmtest ${CMAKE_BINARY_DIR}/fstest ${CMAKE_BINARY_DIR}/pctest ${CMAKE_BINARY_DIR}/bigexec
         ${CMAKE_BINARY_DIR}/ext2test)
else()
    message(WARNING "musl toolchain not found at ${ARC_MUSL_PREFIX}gcc (tools/dev-setup.sh musl): "
                    "qemu.musl, qemu.fpu, qemu.mm, qemu.vfs, qemu.pagecache and qemu.ext2 will fail")
endif()

find_program(MUSL_GCC musl-gcc)
if(MUSL_GCC)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/start_musl_pie
        COMMAND ${MUSL_GCC} -static-pie -fPIE -O2 -Wall -Wextra -Werror -std=c11
                -o ${CMAKE_BINARY_DIR}/start_musl_pie ${_start_src}
        DEPENDS ${_start_src}
        COMMENT "musl-gcc -static-pie start_musl_pie")
    list(APPEND _musl_outputs ${CMAKE_BINARY_DIR}/start_musl_pie)
else()
    message(WARNING "musl-gcc not found (apt install musl-tools): qemu.musl will fail")
endif()

if(_musl_outputs)
    add_custom_target(musl_testprogs ALL DEPENDS ${_musl_outputs})
endif()
