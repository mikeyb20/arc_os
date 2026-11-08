# arc_os — QEMU boot tests (M0-02). Run with `ctest -L qemu`.
#
# Each test boots a test ISO in kernel test mode (arc.test=<suite>) through
# tools/qemu-test.py. They need the cross build first:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake && cmake --build build
# Label `slow` is reserved for long tests (none yet).

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "python3 not found: QEMU tests disabled")
    return()
endif()

set(ARC_CROSS_BUILD_DIR "${CMAKE_SOURCE_DIR}/build" CACHE PATH
    "Cross build directory (kernel.elf, user programs) used by the QEMU tests")

set(_qemu_test ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/qemu-test.py
    --build-dir ${ARC_CROSS_BUILD_DIR})

# Boot, run hello, then the shell non-interactively on smoke.sh: an external
# command, the ls/stat/cd/pwd/run/id builtins (getdents64, stat, wait4 by pid)
add_test(NAME qemu.smoke COMMAND ${_qemu_test} --suite smoke
    --module tests/qemu/suites/smoke.sh
    --expect "arc_os shell"
    --expect "(?s)\\]\\$ .*Hello from exec'd binary"
    --expect "-rw-rw-rw- +[0-9]+ shell"
    --expect "Type: directory"
    --expect "\\[/boot\\]\\$ /boot"
    --expect "exited with status 0"
    --expect "uid=0 gid=0")

# Every existing user program, run once
add_test(NAME qemu.userland COMMAND ${_qemu_test} --suite userland)

# Linux syscall ABI (M0-05): getdents64, stat, wait4, rt_sigaction, ioctl, brk, -ENOSYS
add_test(NAME qemu.abi COMMAND ${_qemu_test} --suite abi --module abi_check)

# Harness self-checks: a failing program is reported by name and reason,
# and a kernel panic ends the run within seconds instead of at the timeout.
add_test(NAME qemu.selftest.fail COMMAND ${_qemu_test} --suite selftest --module testfail)
set_tests_properties(qemu.selftest.fail PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL testfail exit=3")
add_test(NAME qemu.selftest.panic COMMAND ${_qemu_test} --suite kpanic --expect-panic --timeout 30)

set_tests_properties(qemu.smoke qemu.userland qemu.abi qemu.selftest.fail qemu.selftest.panic
    PROPERTIES LABELS qemu TIMEOUT 180 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
