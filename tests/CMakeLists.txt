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
    test_pci.c
    test_virtio.c
    test_string.c
    test_vfs.c
    test_syscall.c
    test_elf.c
    test_fd.c
    test_vmm.c
    test_spinlock.c
    test_gdt.c
    test_idt.c
    test_bootinfo.c
    test_pic.c
    test_user_access.c
    test_tty.c
    test_keyboard.c
    test_shell.c
    test_pipe.c
)

# Include kernel source root for "lib/mem.h" etc, but NOT kernel/include
# (those are freestanding headers that conflict with system headers)
target_include_directories(test_runner PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}/tests
)

target_compile_options(test_runner PRIVATE -Wall -Wextra -std=c11)

# test_bootinfo.c includes limine.c which does #include <limine.h>.
# Use -idirafter so kernel/include is searched AFTER system paths,
# avoiding conflicts with freestanding stdarg.h etc.
set_source_files_properties(test_bootinfo.c PROPERTIES
    COMPILE_FLAGS "-idirafter ${CMAKE_SOURCE_DIR}/kernel/include"
)
target_link_libraries(test_runner PRIVATE pthread)

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
add_test(NAME test_pci     COMMAND test_runner --suite pci)
add_test(NAME test_virtio  COMMAND test_runner --suite virtio)
add_test(NAME test_string  COMMAND test_runner --suite string)
add_test(NAME test_vfs      COMMAND test_runner --suite vfs)
add_test(NAME test_syscall  COMMAND test_runner --suite syscall)
add_test(NAME test_elf      COMMAND test_runner --suite elf)
add_test(NAME test_fd       COMMAND test_runner --suite fd)
add_test(NAME test_vmm      COMMAND test_runner --suite vmm)
add_test(NAME test_spinlock  COMMAND test_runner --suite spinlock)
add_test(NAME test_gdt       COMMAND test_runner --suite gdt)
add_test(NAME test_idt       COMMAND test_runner --suite idt)
add_test(NAME test_bootinfo  COMMAND test_runner --suite bootinfo)
add_test(NAME test_pic          COMMAND test_runner --suite pic)
add_test(NAME test_user_access  COMMAND test_runner --suite user_access)
add_test(NAME test_tty          COMMAND test_runner --suite tty)
add_test(NAME test_keyboard     COMMAND test_runner --suite keyboard)
add_test(NAME test_shell        COMMAND test_runner --suite shell)
add_test(NAME test_pipe         COMMAND test_runner --suite pipe)
