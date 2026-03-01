# arc_os — Host-side unit tests (built with system GCC, not cross-compiler)

add_executable(test_runner
    test_main.c
    test_mem.c
    test_pmm.c
    test_kprintf.c
    test_kmalloc.c
    test_isr.c
    test_thread.c
    test_sched.c
    test_process.c
)

# Include kernel source root for "lib/mem.h" etc, but NOT kernel/include
# (those are freestanding headers that conflict with system headers)
target_include_directories(test_runner PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}/tests
)

target_compile_options(test_runner PRIVATE -Wall -Wextra -std=c11)

# Run all suites
add_test(NAME arc_os_tests COMMAND test_runner)

# Per-suite CTest entries
add_test(NAME test_mem     COMMAND test_runner --suite mem)
add_test(NAME test_pmm     COMMAND test_runner --suite pmm)
add_test(NAME test_kprintf COMMAND test_runner --suite kprintf)
add_test(NAME test_kmalloc COMMAND test_runner --suite kmalloc)
add_test(NAME test_isr     COMMAND test_runner --suite isr)
add_test(NAME test_thread  COMMAND test_runner --suite thread)
add_test(NAME test_sched   COMMAND test_runner --suite sched)
add_test(NAME test_process COMMAND test_runner --suite process)
