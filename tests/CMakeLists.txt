# arc_os — Host-side unit tests (built with system GCC, not cross-compiler)

add_executable(test_runner
    test_main.c
    test_harness.c
    test_mem.c
    test_pmm.c
    test_kprintf.c
    test_panic.c
    test_smbios.c
    test_kmalloc.c
    test_isr.c
    test_thread.c
    test_sched.c
    test_process.c
    test_pci.c
    test_bus.c
    test_irq.c
    test_irq_vector.c
    test_dma.c
    test_virtio.c
    test_gpt.c
    test_blkdev.c
    test_string.c
    test_syscall.c
    test_elf.c
    test_vmm.c
    test_rbtree.c
    test_vma.c
    test_mm.c
    test_spinlock.c
    test_gdt.c
    test_idt.c
    test_bootinfo.c
    test_pic.c
    test_user_access.c
    test_tty.c
    test_keyboard.c
    test_shell.c
    test_signal.c
    test_waitqueue.c
    test_path.c
    test_exec_argv.c
    test_net_util.c
    test_ethernet.c
    test_arp.c
    test_ipv4.c
    test_icmp.c
    test_mutex.c
    test_acpi.c
    test_lapic.c
    test_percpu.c
    test_fb_console.c
    test_passwd.c
    test_socket.c
    test_arc_string.c
    test_arc_printf.c
    test_ansi.c
    test_cmdline.c
    test_qr.c
    test_qr_screen.c
)

# Include kernel source root for "lib/mem.h" etc, but NOT kernel/include
# (those are freestanding headers that conflict with system headers)
target_include_directories(test_runner PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}/tests
)

# kernel/include/uapi (Linux ABI numbers and structs) is reached as "uapi/..." through
# -idirafter, so system headers still win for stdint.h and friends.
target_compile_options(test_runner PRIVATE -Wall -Wextra -std=c11 -DARC_HOST_TEST
    -idirafter ${CMAKE_SOURCE_DIR}/kernel/include)

# test_bootinfo.c includes limine.c which does #include <limine.h>.
# Use -idirafter so kernel/include is searched AFTER system paths,
# avoiding conflicts with freestanding stdarg.h etc.
set_source_files_properties(test_bootinfo.c PROPERTIES
    COMPILE_FLAGS "-idirafter ${CMAKE_SOURCE_DIR}/kernel/include"
)
set_source_files_properties(test_signal.c PROPERTIES
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
add_test(NAME test_bus     COMMAND test_runner --suite bus)
add_test(NAME test_irq     COMMAND test_runner --suite irq)
add_test(NAME test_irq_vector COMMAND test_runner --suite irq_vector)
add_test(NAME test_dma     COMMAND test_runner --suite dma)
add_test(NAME test_virtio  COMMAND test_runner --suite virtio)
add_test(NAME test_gpt     COMMAND test_runner --suite gpt)
add_test(NAME test_blkdev  COMMAND test_runner --suite blkdev)
add_test(NAME test_string  COMMAND test_runner --suite string)
add_test(NAME test_syscall  COMMAND test_runner --suite syscall)
add_test(NAME test_elf      COMMAND test_runner --suite elf)
add_test(NAME test_vmm      COMMAND test_runner --suite vmm)
add_test(NAME test_rbtree   COMMAND test_runner --suite rbtree)
add_test(NAME test_vma      COMMAND test_runner --suite vma)
add_test(NAME test_mm       COMMAND test_runner --suite mm)
add_test(NAME test_spinlock  COMMAND test_runner --suite spinlock)
add_test(NAME test_gdt       COMMAND test_runner --suite gdt)
add_test(NAME test_lapic     COMMAND test_runner --suite lapic)
add_test(NAME test_percpu    COMMAND test_runner --suite percpu)
add_test(NAME test_idt       COMMAND test_runner --suite idt)
add_test(NAME test_bootinfo  COMMAND test_runner --suite bootinfo)
add_test(NAME test_pic          COMMAND test_runner --suite pic)
add_test(NAME test_user_access  COMMAND test_runner --suite user_access)
add_test(NAME test_tty          COMMAND test_runner --suite tty)
add_test(NAME test_keyboard     COMMAND test_runner --suite keyboard)
add_test(NAME test_shell        COMMAND test_runner --suite shell)
add_test(NAME test_signal       COMMAND test_runner --suite signal)
add_test(NAME test_waitqueue    COMMAND test_runner --suite waitqueue)
add_test(NAME test_path         COMMAND test_runner --suite path)
add_test(NAME test_exec_argv   COMMAND test_runner --suite exec_argv)
add_test(NAME test_net_util    COMMAND test_runner --suite net_util)
add_test(NAME test_ethernet    COMMAND test_runner --suite ethernet)
add_test(NAME test_arp         COMMAND test_runner --suite arp)
add_test(NAME test_ipv4        COMMAND test_runner --suite ipv4)
add_test(NAME test_icmp        COMMAND test_runner --suite icmp)
add_test(NAME test_mutex       COMMAND test_runner --suite mutex)
add_test(NAME test_acpi        COMMAND test_runner --suite acpi)
add_test(NAME test_fb_console  COMMAND test_runner --suite fb_console)
add_test(NAME test_passwd      COMMAND test_runner --suite passwd)
add_test(NAME test_socket_net  COMMAND test_runner --suite socket_net)
add_test(NAME test_arc_string  COMMAND test_runner --suite arc_string)
add_test(NAME test_arc_printf  COMMAND test_runner --suite arc_printf)
add_test(NAME test_ansi        COMMAND test_runner --suite ansi)
add_test(NAME test_cmdline     COMMAND test_runner --suite cmdline)
add_test(NAME test_panic       COMMAND test_runner --suite panic)
add_test(NAME test_smbios      COMMAND test_runner --suite smbios)
add_test(NAME test_qr          COMMAND test_runner --suite qr)
add_test(NAME test_qr_screen   COMMAND test_runner --suite qr_screen)
set_tests_properties(arc_os_tests test_gpt test_blkdev PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# M1-11: the VFS suites run the real kernel/fs sources (and proc/fd.c), each
# its own object as in the kernel, over host stand-ins for the rest of the
# kernel (vfs_env.c). Their own program: test_runner's suites include private
# copies of some of these files.
add_library(arc_vfs_host STATIC
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/dcache.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/vfs.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/ramfs.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/pipe.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/devfs.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/procfs.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/fat32.c
    ${CMAKE_SOURCE_DIR}/kernel/fs/path.c
    ${CMAKE_SOURCE_DIR}/kernel/proc/fd.c
    ${CMAKE_SOURCE_DIR}/kernel/lib/rbtree.c
    vfs_env.c
)
target_include_directories(arc_vfs_host PUBLIC ${CMAKE_SOURCE_DIR}/kernel ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(arc_vfs_host PUBLIC -Wall -Wextra -std=c11 -DARC_HOST_TEST
    -idirafter ${CMAKE_SOURCE_DIR}/kernel/include)
add_executable(test_vfs_runner vfs_main.c test_harness.c
    test_vfs.c test_fd.c test_pipe.c test_devfs.c test_procfs.c test_fat32.c test_cred.c)
target_link_libraries(test_vfs_runner PRIVATE arc_vfs_host)
add_test(NAME vfs_tests COMMAND test_vfs_runner)
foreach(_suite vfs fd pipe devfs procfs fat32 cred)
    add_test(NAME test_${_suite} COMMAND test_vfs_runner --suite ${_suite})
endforeach()
set_tests_properties(vfs_tests test_fat32 PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The FAT32 suite reads tests/fat32_test.img (gitignored); build it first.
# Needs mkfs.fat (dosfstools) and mtools, both installed by tools/dev-setup.sh.
add_test(NAME fat32_test_image
    COMMAND bash ${CMAKE_SOURCE_DIR}/tools/make-fat32-test-disk.sh tests/fat32_test.img)
set_tests_properties(fat32_test_image PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    FIXTURES_SETUP fat32_image)
set_tests_properties(test_fat32 vfs_tests PROPERTIES FIXTURES_REQUIRED fat32_image)

# M1-10: GPT images (sgdisk, plus tools/make-gpt-image.py for 4Kn) in tests/gpt/
# (gitignored); the gpt suite also reads the FAT32 image as a disk without GPT.
add_test(NAME gpt_test_images COMMAND bash ${CMAKE_SOURCE_DIR}/tools/make-gpt-test-disk.sh host tests/gpt)
set_tests_properties(gpt_test_images PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    FIXTURES_SETUP gpt_images)
set_tests_properties(arc_os_tests test_gpt test_blkdev PROPERTIES FIXTURES_REQUIRED "fat32_image;gpt_images")

# M0-05: kernel/include/uapi against the host's Linux headers (sizes, offsets,
# syscall numbers, errno and flag values). Its own program: the two header sets
# cannot share a translation unit.
add_executable(test_abi_layout test_abi_layout.c abi_layout_ours.c abi_layout_host_linux.c)
target_include_directories(test_abi_layout PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(test_abi_layout PRIVATE -Wall -Wextra -std=c11)
set_source_files_properties(abi_layout_ours.c PROPERTIES
    COMPILE_FLAGS "-idirafter ${CMAKE_SOURCE_DIR}/kernel/include")
add_test(NAME test_abi_layout COMMAND test_abi_layout)

# Cross build directory (kernel.elf, user programs, hygiene.log) read by the
# hygiene check and the QEMU tests
set(ARC_CROSS_BUILD_DIR "${CMAKE_SOURCE_DIR}/build" CACHE PATH
    "Cross build directory (kernel.elf, user programs) used by the QEMU tests")

# M0-03: tools/archive.py (lock-file checks, bundles, fetch) with file:// URLs only
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME tools_archive COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tools/test_archive.py)
    # M0-04: tools/syscall-inventory.py on hand-written strace lines
    add_test(NAME tools_syscall_inventory
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tools/test_syscall_inventory.py)
    # M2-01: lane helpers (tools/lane/run.sh, tools/lane/sizes.py, lane budget logs)
    add_test(NAME tools_lane COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tools/test_lane_tools.py)
    # M3-01: content-lane scripts (tools/content/catalog.py, budget.py), no network
    add_test(NAME tools_content
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tools/test_content_tools.py)
    # M0-09: tools/hygiene.py on a fixture tree, and hygiene limits as exact ratchets
    add_test(NAME tools_hygiene
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tools/test_hygiene.py)
    # M0-09 (ADR-0020): hygiene counts of the real tree against tests/budgets.txt,
    # plus the syscall-table check. Writes hygiene.log into the cross build
    # directory, so CI's qemu job carries the same numbers.
    add_test(NAME hygiene
        COMMAND sh -c "\"$0\" tools/hygiene.py --out \"$1/hygiene.log\" && \"$0\" tools/budget-check.py --build-dir \"$1\" --only-source hygiene"
            ${Python3_EXECUTABLE} ${ARC_CROSS_BUILD_DIR})
    set_tests_properties(hygiene PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # M0-07: manifest schemas (schemas/) — valid examples pass, invalid ones
    # fail for the reason their name gives. Needs python3-jsonschema.
    add_test(NAME schemas COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/schemas/validate.py)
endif()

# Every test in this directory runs on the build host: `ctest -L host`.
# QEMU boot tests (M0-02) carry the `qemu` label instead.
get_property(_host_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_host_tests} PROPERTIES LABELS host)

# QEMU boot tests live in their own directory so they are not labelled `host`.
add_subdirectory(qemu)
