cmake_minimum_required(VERSION 3.16)
project(arc_os C ASM_NASM)

# NASM outputs elf64 object files
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)

add_subdirectory(kernel)

# Host-side tests — only when building natively (not cross-compiling)
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Convenience targets ---

add_custom_target(iso
    COMMAND bash ${CMAKE_SOURCE_DIR}/tools/make-iso.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Creating bootable ISO"
    VERBATIM
)

add_custom_target(run
    COMMAND bash ${CMAKE_SOURCE_DIR}/tools/run.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building and running arc_os in QEMU"
    VERBATIM
)

add_custom_target(debug
    COMMAND bash ${CMAKE_SOURCE_DIR}/tools/debug.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building and running arc_os in QEMU (debug mode)"
    VERBATIM
)
