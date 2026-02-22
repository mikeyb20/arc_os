# arc_os kernel build configuration

# --- Assembly sources (NASM) ---
set(KERNEL_ASM_SOURCES
    arch/x86_64/entry.asm
)

# --- C sources ---
set(KERNEL_C_SOURCES
    boot/limine_requests.c
    boot/limine.c
    boot/kmain.c
    lib/mem.c
    lib/kprintf.c
    arch/x86_64/serial.c
)

set(KERNEL_SOURCES ${KERNEL_ASM_SOURCES} ${KERNEL_C_SOURCES})

# Only build kernel.elf when there are source files
if(KERNEL_SOURCES)
    add_executable(kernel.elf ${KERNEL_SOURCES})

    # NASM flags
    target_compile_options(kernel.elf PRIVATE
        $<$<COMPILE_LANGUAGE:ASM_NASM>:-f elf64>
    )

    # Include the kernel root so headers use paths like "mm/pmm.h"
    target_include_directories(kernel.elf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    # Linker flags — freestanding, use our linker script
    set_target_properties(kernel.elf PROPERTIES
        LINK_FLAGS "-nostdlib -T ${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/linker.ld"
    )

    # Copy kernel.elf to build root for ISO creation
    add_custom_command(TARGET kernel.elf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kernel.elf> ${CMAKE_BINARY_DIR}/kernel.elf
        COMMENT "Copying kernel.elf to build directory"
    )
else()
    message(STATUS "No kernel sources yet — skipping kernel.elf target")
endif()
