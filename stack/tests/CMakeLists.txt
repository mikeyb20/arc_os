# Lane tests (label `lane`). M2-01: the M0-04 target programs, built with the pinned
# musl toolchain, pass on Linux inside the syscall inventory.

set(ARC_TARGET_DIR "${ARC_ROOT}/tests/target")

add_executable(libc_basic "${ARC_TARGET_DIR}/libc_basic.c")
add_executable(pthread_basic "${ARC_TARGET_DIR}/pthread_basic.c")
target_link_libraries(pthread_basic PRIVATE pthread)
add_executable(cxx_basic "${ARC_TARGET_DIR}/cxx_basic.cpp")
target_link_libraries(cxx_basic PRIVATE pthread)

lane_test(NAME libc_basic COMMAND $<TARGET_FILE:libc_basic>)
lane_test(NAME pthread_basic COMMAND $<TARGET_FILE:pthread_basic>)
lane_test(NAME cxx_basic COMMAND $<TARGET_FILE:cxx_basic>)

# Sizes for stack/budgets.txt (ADR-0019): loaded ELF bytes, as kernel_image_bytes.
lane_driver_test(NAME sizes COMMAND python3 "${ARC_ROOT}/tools/lane/sizes.py"
    "${ARC_LANE_BUDGET_DIR}/sizes.budget.log" cxx_basic_bytes=$<TARGET_FILE:cxx_basic>)
