# arc_os — Host-side unit tests (built with system GCC, not cross-compiler)

add_executable(test_runner
    test_main.c
    test_mem.c
    test_pmm.c
)

# Include kernel source root for "lib/mem.h" etc, but NOT kernel/include
# (those are freestanding headers that conflict with system headers)
target_include_directories(test_runner PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel
)

target_compile_options(test_runner PRIVATE -Wall -Wextra -std=c11)

add_test(NAME arc_os_tests COMMAND test_runner)
