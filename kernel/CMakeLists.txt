# arc_os kernel build configuration

# Kernel only builds with the cross-compiler
if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

# --- Assembly sources (NASM) ---
set(KERNEL_ASM_SOURCES
    arch/x86_64/entry.asm
    arch/x86_64/gdt_flush.asm
    arch/x86_64/isr_stubs.asm
    arch/x86_64/context_switch.asm
    arch/x86_64/syscall_entry.asm
    arch/x86_64/usermode.asm
    arch/x86_64/selftest_avx2.asm
    arch/x86_64/uaccess.asm
)

# --- C sources ---
set(KERNEL_C_SOURCES
    boot/limine_requests.c
    boot/limine.c
    boot/kmain.c
    lib/mem.c
    lib/string.c
    lib/kprintf.c
    lib/klog.c
    lib/ksyms.c
    lib/panic.c
    lib/cmdline.c
    lib/testmode.c
    lib/crc32.c
    lib/rbtree.c
    arch/x86_64/debug_exit.c
    arch/x86_64/serial.c
    arch/x86_64/gdt.c
    arch/x86_64/idt.c
    arch/x86_64/isr.c
    arch/x86_64/pic.c
    arch/x86_64/pit.c
    arch/x86_64/tsc.c
    mm/pmm.c
    mm/vmm.c
    mm/uaccess.c
    mm/radix.c
    mm/filemap.c
    mm/filemap_mmap.c
    mm/vma.c
    mm/mm.c
    mm/fault.c
    mm/mm_syscalls.c
    mm/slab.c
    mm/kmalloc.c
    arch/x86_64/syscall.c
    arch/x86_64/cpu.c
    arch/x86_64/fpu.c
    arch/x86_64/lapic.c
    arch/x86_64/ioapic.c
    arch/x86_64/percpu.c
    arch/x86_64/smp.c
    arch/x86_64/ipi.c
    arch/x86_64/irq_vector.c
    arch/x86_64/tlb.c
    proc/thread.c
    proc/sched.c
    proc/kernel_lock.c
    proc/lockdep.c
    proc/process.c
    proc/proc_syscalls.c
    proc/elf.c
    proc/exec_stack.c
    proc/init.c
    proc/fd.c
    proc/signal.c
    proc/waitqueue.c
    proc/mutex.c
    proc/rwsem.c
    proc/semaphore.c
    proc/condvar.c
    drivers/acpi.c
    drivers/pci.c
    drivers/pci_irq.c
    drivers/bus.c
    drivers/irq.c
    drivers/mmio.c
    drivers/dma.c
    drivers/smbios.c
    drivers/virtio.c
    drivers/virtio_blk.c
    drivers/blkdev.c
    drivers/tty.c
    drivers/keyboard.c
    drivers/fb_console.c
    drivers/font8x16.c
    drivers/ansi.c
    drivers/vt.c
    fs/vfs.c
    fs/inode.c
    fs/dcache.c
    fs/file.c
    fs/fs_syscalls.c
    fs/ramfs.c
    fs/pipe.c
    fs/fat32.c
    fs/devfs.c
    fs/procfs.c
    fs/path.c
    fs/passwd.c
    net/net_util.c
    net/netif.c
    net/ethernet.c
    net/arp.c
    net/ipv4.c
    net/icmp.c
    net/net.c
    net/udp.c
    net/tcp.c
    net/socket.c
    net/net_syscalls.c
    net/loopback.c
    security/hardening.c
    drivers/virtio_net.c
    diag/report.c
    diag/selftest.c
    diag/bench.c
    diag/qr.c
    diag/qr_text.c
    diag/devtest.c
    diag/qr_screen.c
    diag/qr_mode.c
)

# The build commit (M1-04c): rewritten on every build, but only when the
# commit changes (cmake/build-commit.cmake), so a clean tree recompiles nothing
set(_build_commit_c ${CMAKE_CURRENT_BINARY_DIR}/build_commit.c)
add_custom_target(kernel_build_commit
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUT=${_build_commit_c}
            -P ${CMAKE_SOURCE_DIR}/cmake/build-commit.cmake
    BYPRODUCTS ${_build_commit_c}
    COMMENT "Recording the build commit")

set(KERNEL_SOURCES ${KERNEL_ASM_SOURCES} ${KERNEL_C_SOURCES} ${_build_commit_c})

# Only build kernel.elf when there are source files
if(KERNEL_SOURCES)
    # Every kernel object is compiled once and linked twice (M1-04): the first
    # link gives the function addresses for the panic backtrace's symbol table,
    # the second adds that table in its own section after .rodata, so no
    # function moves (tools/gen-ksyms.py --check proves it on every build).
    add_library(kernel_objs OBJECT ${KERNEL_SOURCES})
    add_dependencies(kernel_objs kernel_build_commit)

    # NASM flags
    target_compile_options(kernel_objs PRIVATE
        $<$<COMPILE_LANGUAGE:ASM_NASM>:-f elf64>
    )

    # Kernel C never touches x87/SSE/AVX registers: they hold the running
    # thread's user state, saved only at context switch (ADR-0003, fpu.h).
    # Frame pointers stay on at every optimisation level: the panic screen
    # walks them for its backtrace (M1-04).
    target_compile_options(kernel_objs PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only -fno-omit-frame-pointer>
    )

    # Debug checks (ARC_DEBUG=1): slab red zones, poison and double-free
    # checks (M1-02b); lock-order checks from M1-05. On while arc_os is in
    # development; a release build turns it off with -DARC_KERNEL_DEBUG=OFF.
    option(ARC_KERNEL_DEBUG "Build the kernel with debug checks" ON)
    if(ARC_KERNEL_DEBUG)
        target_compile_definitions(kernel_objs PRIVATE ARC_DEBUG=1)
    else()
        target_compile_definitions(kernel_objs PRIVATE ARC_DEBUG=0)
    endif()

    # Include the kernel root so headers use paths like "mm/pmm.h"
    target_include_directories(kernel_objs PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_gen_ksyms ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen-ksyms.py --nm ${CMAKE_NM})
    set(_ksyms_empty ${CMAKE_CURRENT_BINARY_DIR}/ksyms_empty.c)
    set(_ksyms ${CMAKE_CURRENT_BINARY_DIR}/ksyms.c)
    add_custom_command(OUTPUT ${_ksyms_empty}
        COMMAND ${_gen_ksyms} --empty --out ${_ksyms_empty}
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen-ksyms.py
        COMMENT "Generating the empty kernel symbol table")

    set(_kernel_link_flags "-nostdlib -T ${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/linker.ld")

    add_executable(kernel_stage1.elf $<TARGET_OBJECTS:kernel_objs> ${_ksyms_empty})
    set_target_properties(kernel_stage1.elf PROPERTIES LINK_FLAGS "${_kernel_link_flags}"
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/linker.ld)

    add_custom_command(OUTPUT ${_ksyms}
        COMMAND ${_gen_ksyms} --out ${_ksyms} $<TARGET_FILE:kernel_stage1.elf>
        DEPENDS kernel_stage1.elf ${CMAKE_SOURCE_DIR}/tools/gen-ksyms.py
        COMMENT "Generating the kernel symbol table")

    add_executable(kernel.elf $<TARGET_OBJECTS:kernel_objs> ${_ksyms})
    target_include_directories(kernel_stage1.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(kernel.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_target_properties(kernel.elf PROPERTIES LINK_FLAGS "${_kernel_link_flags}"
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/linker.ld)

    # Check no function moved, then copy kernel.elf to the build root for the ISO
    add_custom_command(TARGET kernel.elf POST_BUILD
        COMMAND ${_gen_ksyms} --check $<TARGET_FILE:kernel_stage1.elf> $<TARGET_FILE:kernel.elf>
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kernel.elf> ${CMAKE_BINARY_DIR}/kernel.elf
        COMMENT "Checking symbol addresses and copying kernel.elf to build directory"
    )
else()
    message(STATUS "No kernel sources yet — skipping kernel.elf target")
endif()
