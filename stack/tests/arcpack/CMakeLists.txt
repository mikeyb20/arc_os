# Lane tests for libarcpack (M3-02): the three-document pack, damaged packs, a document past the
# 8 MiB window and the 200 000-chunk lookup run. arcpack_lane.py says what each test does; arcpack_test.c is the program it drives.
add_executable(arcpack_test arcpack_test.c)
target_link_libraries(arcpack_test PRIVATE arcpack)
foreach(test three damaged long big)
    lane_driver_test(NAME arcpack_${test} COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/arcpack_lane.py" ${test}
        --program $<TARGET_FILE:arcpack_test> --validate "${ARC_ROOT}/tests/schemas/validate.py")
endforeach()
